/*
 *  Module to retrieve point clouds from SFM module and dump them to PCL
 *  compatible files. Interfaces with OPC to retrieve the position
 *  of a desired object and retrieves a point cloud from SFM.
 *
 *  Author: Fabrizio Bottarel - <fabrizio.bottarel@iit.it>
 *
 */

#include <yarp/os/all.h>
#include <yarp/sig/all.h>
#include <yarp/dev/all.h>
#include <yarp/math/Math.h>

#include <string>
#include <iostream>
#include <fstream>
#include <deque>

using namespace std;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::math;

/**************************************************************/
class Point3DRGB
{
public:
    double x, y, z;
    int r, g, b;

    Point3DRGB(double x, double y, double z, char r, char g, char b)
    {
        this->x = x;
        this->y = y;
        this->z = z;
        this->r = g;
        this->g = g;
        this->b = b;
    }

    Vector toYarpVectorRGB()
    {
        Vector point_vec(6);

        point_vec(0) = x;
        point_vec(1) = y;
        point_vec(2) = z;
        point_vec(3) = r;
        point_vec(4) = g;
        point_vec(5) = b;

        return point_vec;
    }
};

class PointCloudReadModule: public yarp::os::RFModule
{
protected:

    RpcServer inCommandPort;
    RpcClient outCommandOPC;
    RpcClient outCommandSFM;
    RpcClient outCommandSegm;

    BufferedPort< Matrix > outPort;
    BufferedPort<ImageOf<PixelRgb>> inImgPort;

    Mutex mutex;

    string moduleName, operationMode, objectToFind, baseDumpFileName;

    bool retrieveObjectBoundingBox(const string objName, Vector &top_left_xy, Vector &bot_right_xy)
    {
        //  get object bounding box from OPC module given object name

        top_left_xy.resize(2);
        bot_right_xy.resize(2);

        //  command message format: [ask] (("prop0" "<" <val0>) || ("prop1" ">=" <val1>) ...)
        Bottle cmd, reply;
        cmd.addVocab(Vocab::encode("ask"));
        Bottle &content = cmd.addList().addList();
        content.addString("name");
        content.addString("==");
        content.addString(objName);
        outCommandOPC.write(cmd,reply);

        //  reply message format: [nack]; [ack] ("id" (<num0> <num1> ...))
        if (reply.size()>1)
        {
            //  verify that first element is "ack"
            if (reply.get(0).asVocab() == Vocab::encode("ack"))
            {
                //  get list of all id's of objects named objName
                if (Bottle *idField = reply.get(1).asList())
                {
                    if (Bottle *idValues = idField->get(1).asList())
                    {
                        //  if there are more objects under the same name, pick the first one
                        int id = idValues->get(0).asInt();

                        //  get the actual bounding box
                        //  command message format:  [get] (("id" <num>) (propSet ("prop0" "prop1" ...)))
                        cmd.clear();
                        cmd.addVocab(Vocab::encode("get"));
                        Bottle &content = cmd.addList();
                        Bottle &list_bid = content.addList();
                        list_bid.addString("id");
                        list_bid.addInt(id);
                        Bottle &list_propSet = content.addList();
                        list_propSet.addString("propSet");
                        Bottle &list_items = list_propSet.addList();
                        list_items.addString("position_2d_left");
                        Bottle replyProp;
                        outCommandOPC.write(cmd,replyProp);

                        //reply message format: [nack]; [ack] (("prop0" <val0>) ("prop1" <val1>) ...)
                        if (replyProp.get(0).asVocab() == Vocab::encode("ack"))
                        {
                            if (Bottle *propField = replyProp.get(1).asList())
                            {
                                if (Bottle *position_2d_bb = propField->find("position_2d_left").asList())
                                {
                                    //  position_2d_left contains x,y of top left and x,y of bottom right
                                    top_left_xy(0)  = position_2d_bb->get(0).asInt();
                                    top_left_xy(1)  = position_2d_bb->get(1).asInt();
                                    bot_right_xy(0) = position_2d_bb->get(2).asInt();
                                    bot_right_xy(1) = position_2d_bb->get(3).asInt();
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }

        yError() << "RPC to OPC returned unexpected reply";
        return false;

    }

    bool retrieveObjectPointCloud(deque<Point3DRGB> &objectPointCloud)
    {
        //  get object bounding box given object name

        Vector bb_top_left, bb_bot_right;

        if (retrieveObjectBoundingBox(objectToFind, bb_top_left, bb_bot_right))
        {
            int width   = abs(bb_bot_right(0) - bb_top_left(0)) + 1;
            int height  = abs(bb_bot_right(1) - bb_top_left(1)) + 1;

            //  get list of points that belong to the object from lbpextract
            //  command message format: [get_component_around x y]
            int center_bb_x = bb_top_left(0) + width/2;
            int center_bb_y = bb_top_left(1) + (bb_bot_right(1) - bb_top_left(1))/2;

            Bottle cmdSeg, replySeg;
            cmdSeg.addString("get_component_around");
            cmdSeg.addInt(center_bb_x);
            cmdSeg.addInt(center_bb_y);

            if (!outCommandSegm.write(cmdSeg,replySeg)){
                yError() << "Could not write to segmentation RPC port";
                return false;
            }

            //  lbpExtract replies with a list of points
            Bottle *pointList = replySeg.get(0).asList();

            if (pointList->size() > 0){

                //  each point is a list of 2 coordinates
                //  build one list of points to query SFM for 3d coordinates
                Bottle cmdSFM, replySFM;
                cmdSFM.addString("Points");
                cmdSFM.addString(pointList->toString());

                if (!outCommandSFM.write(cmdSFM, replySFM))
                {
                    yError() << "Could not write to SFM RPC port";
                    return false;
                }

                //  acquire image from camera input
                ImageOf<PixelRgb> *inCamImg = inImgPort.read();

                //  empty point cloud
                objectPointCloud.clear();

                yDebug() << "Reply from SFM obtained. Cycling through " << pointList->size() << " 3D points...";

                for (int point_idx = 0; point_idx < pointList->size(); point_idx++)
                {
                    double x = replySFM.get(point_idx*3+0).asDouble();   // x value
                    double y = replySFM.get(point_idx*3+1).asDouble();   // y value
                    double z = replySFM.get(point_idx*3+2).asDouble();   // z value

                    //  0 0 0 points are invalid and must be discarded
                    if (x==0 && y==0 && z==0)
                        continue;

                    //  fetch rgb from image according to 2D coordinates
                    Bottle *point2D = pointList->get(point_idx).asList();
                    PixelRgb point_rgb = inCamImg->pixel(point2D->get(0).asInt(), point2D->get(1).asInt());

                    objectPointCloud.push_back(Point3DRGB(x, y, z, point_rgb.r, point_rgb.g, point_rgb.b));
                }

                yInfo() << "Point cloud retrieved: " << objectPointCloud.size() << " points stored.";

            }
            return true;
        }
        else
        {
            yError() << "Could not retrieve bounding box for object " << objectToFind;
            return false;
        }
    }


    bool retrieveObjectPointCloud(Matrix &objectPointCloud)
    {
        //  get object bounding box given object name

        Vector bb_top_left, bb_bot_right;

        if (retrieveObjectBoundingBox(objectToFind, bb_top_left, bb_bot_right))
        {
            int width   = abs(bb_bot_right(0) - bb_top_left(0)) + 1;
            int height  = abs(bb_bot_right(1) - bb_top_left(1)) + 1;

            //  get point cloud from SFM by querying it with a bounding box
            //  command message format: [Rect tlx tly w h step]
            Bottle cmd, reply;
            cmd.addString("Rect");
            cmd.addInt(bb_top_left(0));
            cmd.addInt(bb_top_left(1));
            cmd.addInt(width);
            cmd.addInt(height);
            cmd.addInt(1);

            outCommandSFM.write(cmd, reply);

            //  point cloud has the form of a n x 3 matrix
            objectPointCloud.resize(width*height, 3);
            objectPointCloud.zero();

            //  reply contains a list of X Y Z triplets
            //  WARNING: LOGS INVALID DISPARITY POINTS AS WELL
            for (int idx = 0; idx < reply.size(); idx+=3)
            {
                objectPointCloud(idx/3, 0) = reply.get(idx+0).asDouble();   // x value
                objectPointCloud(idx/3, 1) = reply.get(idx+1).asDouble();   // y value
                objectPointCloud(idx/3, 2) = reply.get(idx+2).asDouble();   // z value
            }
            return true;
        }
        else
        {
            yError() << "Could not retrieve bounding box for object " << objectToFind;
            return false;
        }

    }

    bool streamSinglePointCloud()
    {
        //  retrieve object point cloud
        deque<Point3DRGB> pointCloud;

        Matrix &matPointCloud = outPort.prepare();
        matPointCloud.resize(pointCloud.size(), 6);
        matPointCloud.zero();

        if (retrieveObjectPointCloud(pointCloud))
        {
            //  send point cloud on output port
            for (int idx_point = 0; idx_point < pointCloud.size(); idx_point++)
            {
                //  convert to yarp vector and append as rowbefore writing to output
                matPointCloud.setRow(idx_point, pointCloud.at(idx_point).toYarpVectorRGB());
                yDebug() << "Point: " << matPointCloud(idx_point, 0) << " " << matPointCloud(idx_point, 1) << " " << matPointCloud(idx_point, 2);
            }

            //  farewell, ol' point cloud
            outPort.write();
            return true;
        }
        else
        {
            yError() << "Could not retrieve point cloud for object " << objectToFind;
            return false;
        }
    }

//    int dumpToPCDFile(const string &filename, const Matrix &pointCloud)
//    {
//        fstream dumpFile;
//        dumpFile.open(filename + ".pcd", ios::out);

//        if (dumpFile.is_open()){
//            int n_points = pointCloud.rows();

//            dumpFile << "# .PCD v.7 - Point Cloud Data file format" << "\n";
//            dumpFile << "VERSION .7" << "\n";
//            dumpFile << "FIELDS x y z" << "\n";             //  suppose point cloud contains x y z coordinates
//            dumpFile << "SIZE 8 8 8" << "\n";               //  suppose each entry is double
//            dumpFile << "TYPE F F F" << "\n";
//            dumpFile << "COUNT 1 1 1" << "\n";
//            dumpFile << "WIDTH " << n_points << "\n";
//            dumpFile << "HEIGHT 1" << "\n";
//            dumpFile << "VIEWPOINT 0 0 0 1 0 0 0" << "\n";
//            dumpFile << "POINTS " << n_points << "\n";

//            outPort.write();
//            return true;        dumpFile << "DATA ascii" << "\n";               //  coordinates will be inserted as ascii and not binary

//            for (int idx_point = 0; idx_point < n_points; idx_point++)
//            {
//                dumpFile << pointCloud(idx_point, 0) << " ";
//                dumpFile << pointCloud(idx_point, 1) << " ";
//                dumpFile << pointCloud(idx_point, 2) << "\n";
//            }

//            dumpFile.close();

//            return 0;
//        }

//        return -1;

//    }

    int dumpToOFFFile(const string &filename, deque<Point3DRGB> &pointCloud)
    {
        fstream dumpFile;
        dumpFile.open(filename + ".off", ios::out);

        if (dumpFile.is_open()){
            int n_points = pointCloud.size();

            dumpFile << "OFF"               << "\n";
            dumpFile << n_points << " 0 0"  << "\n";

            for (int idx_point = 0; idx_point < n_points; idx_point++)
            {
                dumpFile << pointCloud.at(idx_point).toYarpVectorRGB().toString() << "\n";
            }

            dumpFile.close();

            return 0;
        }

        return -1;

    }

public:

    bool configure(ResourceFinder &rf)
    {
        moduleName = "pointCloudRead";

        /*
         *
         *
         * DEBUG INSTRUCTION: ALWAYS LOOK FOR CARS
         *
         *
         */
        objectToFind = "Car";
        baseDumpFileName = "point_cloud";

        bool okOpen = true;

        okOpen &= inCommandPort.open("/" + moduleName + "/rpc");
        okOpen &= inImgPort.open("/" + moduleName + "/imgL:i");
        okOpen &= outCommandOPC.open("/" + moduleName + "/OPCrpc");
        okOpen &= outCommandSFM.open("/" + moduleName + "/SFMrpc");
        okOpen &= outCommandSegm.open("/" + moduleName + "/segmrpc");
        okOpen &= outPort.open("/" + moduleName + "/pointCloud:o");

        if (!okOpen)
        {
            yError() << "Unable to open ports!";
            return false;
        }

        attach(inCommandPort);

        operationMode = "none";

        return true;

    }

    bool interruptModule()
    {
        inCommandPort.interrupt();
        inImgPort.interrupt();
        outCommandOPC.interrupt();
        outCommandSFM.interrupt();
        outCommandSegm.interrupt();

        return true;

    }

    bool close()
    {
        inCommandPort.close();
        inImgPort.close();
        outCommandOPC.close();
        outCommandSFM.close();
        outCommandSegm.close();
        outPort.close();

        return true;

    }

    bool respond(const Bottle &command, Bottle &reply)
    {
        //  change operation mode based on command and available modes
        mutex.lock();

        string modeCmd = command.get(0).asString();

        if (modeCmd == "stream_one")
        {
            operationMode = "stream_one";
            reply.addString("ack");
        }
        else if (modeCmd == "stream_many")
        {
            operationMode = "stream_many";
            reply.addString("ack");
        }
        else if (modeCmd == "dump_one")
        {
            operationMode = "dump_one";
            reply.addString("ack");
        }
        else if (modeCmd == "stop_stream")
        {
            operationMode = "none";
            reply.addString("ack");
        }
        else if (modeCmd == "help")
        {
            reply.addString("Available commands:");
            reply.addString("- stream_one");
            reply.addString("- stream_many");
            reply.addString("- dump_one");
            reply.addString("- stop_stream");
            reply.addString("- quit");
        }
        else if (modeCmd == "quit")
        {
            mutex.unlock();
            return RFModule::respond(command, reply);
        }
        else
            reply.addString("Invalid command. Type help for available commands");

        mutex.unlock();

        return true;

    }

    double getPeriod()
    {
        return 1.0;
    }

    bool updateModule()
    {
        mutex.lock();

        if (operationMode == "stream_one")
        {
            streamSinglePointCloud();
            operationMode = "none";
        }
        else if (operationMode == "stream_many")
        {
            streamSinglePointCloud();
        }
        else if (operationMode == "dump_one")
        {
            deque<Point3DRGB> yarpCloud;

            if (retrieveObjectPointCloud(yarpCloud))
            {
                //  dump point cloud to file
                string dumpFileName = objectToFind + "_" + baseDumpFileName;
//                if (dumpToPCDFile(dumpFileName, yarpCloud) == 0)
//                {
//                    yDebug() << "Dumped point cloud in PCD format: " << dumpFileName;

//                }
                if (dumpToOFFFile(dumpFileName, yarpCloud) == 0)
                {
                    yDebug() << "Dumped point cloud in OFF format: " << dumpFileName;

                }
                else
                    yError() << "Dump failed!";
            }
            else
                yError() << "Could not retrieve object point cloud";

            operationMode = "none";
        }
        else if (operationMode == "none")
        {
            //  birds chirping
        }

        mutex.unlock();

        return true;

    }

};

int main(int argc, char *argv[]) {

    Network yarp;

    if (!yarp.checkNetwork())
    {
        yError()<<"YARP doesn't seem to be available";
        return 1;
    }

    ResourceFinder rf;
    rf.configure(argc, argv);

    PointCloudReadModule pcmod;
    return pcmod.runModule(rf);

}
