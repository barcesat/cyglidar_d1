#include <ros/ros.h>
#include <cyglidar_pcl.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/transforms.h>
#include <pcl_ros/point_cloud.h>
#include <sensor_msgs/PointCloud2.h>
#include <std_msgs/UInt16.h>
#include <boost/asio.hpp>
#include <cmath>
#include <thread>

#define PAYLOAD_SIZE            6

#define DATABUFFER_SIZE_2D      249
#define DATASET_SIZE_2D         121
#define BASE_DEPTH_3D           3000
#define BASE_DEPTH_2D           16000
#define BASE_ANGLE_2D           120

#define DATABUFFER_SIZE_3D      14407
#define DATASET_SIZE_3D         9600

#define COLOR_MIN               0
#define COLOR_MAX               255

#define RIGHT_ANGLE             90
#define HALF_ANGLE              180
#define MATH_PI                 3.14159265

#define DIVISOR                 0.001
#define FOCAL_LENGTH            40.5
#define HORIZONTAL_ANGLE        120
#define VERTITAL_ANGLE          65

const float RADIAN = MATH_PI / HALF_ANGLE;
const float DEGREE = HALF_ANGLE / MATH_PI;

typedef pcl::PointCloud<pcl::PointXYZRGBA> PointXYZRGBA;
pcl::PointCloud<pcl::PointXYZRGBA>::Ptr scan_2D;
pcl::PointCloud<pcl::PointXYZRGBA>::Ptr scan_3D;
tf::TransformListener *tf_listener;

ros::Publisher pub;
int version_num;

uint8_t *cloudBuffer = new uint8_t[DATABUFFER_SIZE_3D - PAYLOAD_SIZE];
float *cloudSetBuffer = new float[DATASET_SIZE_3D];
uint8_t *cloudBuffer_2D = new uint8_t[DATABUFFER_SIZE_2D - PAYLOAD_SIZE];
float *cloudSetBuffer_2D = new float[DATASET_SIZE_2D];

uint8_t* bufferPtr_2D;
uint8_t* bufferPtr01;
uint8_t* bufferPtr02;

uint8_t FIRST, SECOND, THIRD, LSB, MSB;

float centerX_3D, centerY_3D, width, height;
float actualX = 0.0, actualY = 0.0;

float angleRadian = RADIAN * -RIGHT_ANGLE;

float data1, data2, actualDistance, length;
uint8_t currentBuffer;

static std::vector<uint32_t> colorArray;
uint32_t rgb_setup;
uint8_t r_setup, g_setup, b_setup;
void colorBuffer()
{
    int colors = 0;
    r_setup = 254;
    g_setup = 0;
    b_setup = 0;

    for (int colorSet = 0; colorSet < 2; colorSet++)
    {
        for (int i = 0; i < 3; i++)
        {
            for (int colorCnt = 0; colorCnt < COLOR_MAX; colorCnt++)
            {
                switch (colors)
                {
                    // RED -> YELLOW
                    case 0:
                        g_setup++;
                        break;
                    // YELLOW -> BLUE
                    case 1:
                        r_setup--;
                        g_setup--;
                        b_setup++;
                        break;
                    // BLUE -> RED
                    case 2:
                        r_setup++;
                        b_setup--;
                        break;
                }

                rgb_setup = ((uint32_t)r_setup << 16 | (uint32_t)g_setup << 8 | (uint32_t)b_setup);
                colorArray.push_back(rgb_setup);

                if (colorCnt == COLOR_MAX - 1)
                {
                    colors++;
                }
            }
        }
    }

    ROS_INFO("COLORS STORED (%d)", colorArray.size());
}

bool drawing = false;
int distanceCnt_2D;
float angleStep_2D, point_angle_var_2D, point_angle_2D;
float tempX_2D, tempY_2D;
uint8_t cloudScatter_2D()
{
    if (!drawing)
    {
        drawing = true;
        memcpy(cloudBuffer_2D, &bufferPtr_2D[PAYLOAD_SIZE], sizeof(uint8_t) * DATABUFFER_SIZE_2D);

        distanceCnt_2D = 0;
        angleStep_2D = 0;

        for (int dataLength = 0; dataLength < (DATABUFFER_SIZE_2D - PAYLOAD_SIZE) - 1; dataLength+=2)
        {
            MSB = cloudBuffer_2D[dataLength];
            LSB = cloudBuffer_2D[dataLength + 1];

            data1 = (float)((MSB << 8) | LSB);

            cloudSetBuffer_2D[distanceCnt_2D++] = data1;
        }

        point_angle_var_2D = 0.0;
        angleStep_2D = ((float)BASE_ANGLE_2D / (float)DATASET_SIZE_2D);

        for (int idx = 0; idx < DATASET_SIZE_2D; idx++)
        {
            point_angle_2D = (float)(((BASE_ANGLE_2D / 2 * -1) + point_angle_var_2D) * RADIAN);
            point_angle_var_2D += angleStep_2D;

            actualDistance = (cloudSetBuffer_2D[idx]);
            actualX = (sin(point_angle_2D) * actualDistance);
            actualY = (cos(point_angle_2D) * actualDistance); // depth

            tempX_2D = actualX;
            tempY_2D = actualY;
            actualX = (tempX_2D * cos(angleRadian)) + (tempY_2D * -sin(angleRadian));
            actualY = (tempX_2D * sin(angleRadian)) + (tempY_2D * cos(angleRadian));

            scan_2D.get()->points[idx].x = actualX * DIVISOR;
            scan_2D.get()->points[idx].y = actualY * DIVISOR;
            scan_2D.get()->points[idx].z = 0.0;
            
            if (cloudSetBuffer_2D[idx] < (float)BASE_DEPTH_2D)
            {
                scan_2D.get()->points[idx].r = 255;
                scan_2D.get()->points[idx].g = 255;
                scan_2D.get()->points[idx].b = 0;
                scan_2D.get()->points[idx].a = 255;
            }
            else
            {
                scan_2D.get()->points[idx].a = 0;
            }
        }

        drawing = false; 
    }
}

int distanceCnt_3D, index_3D;
float tempX_3D, tempY_3D, tempD_3D;
float x_3D, y_3D, h_3D, fh_3D, dRatio_3D, aRatio_3D, focalRatio_3D, tanA1_3D, tanA2_3D;
float verticalA, horizontalA, verticalA_Single, horizontalA_Half, originalA_H, originalA_V, differenceA_H, differenceA_V;
uint32_t rgb_3D;
uint8_t cloudScatter_3D()
{
    if (!drawing)
    {
        drawing = true;

        switch (currentBuffer)
        {
            case 0x00:
                memcpy(cloudBuffer, &bufferPtr02[PAYLOAD_SIZE], sizeof(uint8_t) * DATABUFFER_SIZE_3D);
                break;
            case 0x01:
                memcpy(cloudBuffer, &bufferPtr01[PAYLOAD_SIZE], sizeof(uint8_t) * DATABUFFER_SIZE_3D);
                break;
        }
        
        distanceCnt_3D = 0;
        for (int dataLength = 0; dataLength < (DATABUFFER_SIZE_3D - PAYLOAD_SIZE) - 1; dataLength+=3)
        {
            FIRST = cloudBuffer[dataLength];
            SECOND = cloudBuffer[dataLength + 1];
            THIRD = cloudBuffer[dataLength + 2];

            data1 = (float)((FIRST << 4) | (SECOND >> 4));
            data2 = (float)(((SECOND & 0xf) << 8) | THIRD);

            cloudSetBuffer[distanceCnt_3D++] = data1;
            cloudSetBuffer[distanceCnt_3D++] = data2;
        }

        horizontalA_Half = (float)HORIZONTAL_ANGLE / 2;
        verticalA_Single = (float)VERTITAL_ANGLE / centerX_3D;
        originalA_H = (atan(centerX_3D / FOCAL_LENGTH) * (HALF_ANGLE / MATH_PI));
        originalA_V = (atan(centerY_3D / FOCAL_LENGTH) * (HALF_ANGLE / MATH_PI));
        differenceA_H = (horizontalA_Half / originalA_H);
        differenceA_V = ((VERTITAL_ANGLE / 2) / originalA_V);
        
        index_3D = 0;
        for (int yy = 0; yy < height; yy++)
        {
            for (int xx = 0; xx < width; xx++)
            {
                index_3D = (xx + (yy * width));

                if (cloudSetBuffer[index_3D] < (float)BASE_DEPTH_3D)
                {
                    x_3D = abs(xx - centerX_3D);
                    y_3D = abs(yy - centerY_3D);
                    
                    tanA1_3D = y_3D / x_3D;
                    h_3D = (y_3D == 0 ? x_3D : (y_3D / sin(atan(tanA1_3D))));

                    tanA2_3D = FOCAL_LENGTH / h_3D;
                    tempD_3D = FOCAL_LENGTH / sin(atan(tanA2_3D));

                    dRatio_3D = (cloudSetBuffer[index_3D] / tempD_3D);
                    actualDistance = (FOCAL_LENGTH * dRatio_3D);

                    actualX = (xx - centerX_3D) * (actualDistance / FOCAL_LENGTH) * differenceA_H;
                    actualY = -(yy - centerY_3D) * (actualDistance / FOCAL_LENGTH) * differenceA_V;
                    
                    // Determine a cloud color based on the distance
                    rgb_3D = colorArray[(int)actualDistance % colorArray.size()];
                    scan_3D.get()->points[index_3D].rgb = *reinterpret_cast<float*>(&rgb_3D);
                    scan_3D.get()->points[index_3D].a = 255;

                    // Rotate the axis of clouds to the right
                    tempX_3D = actualX;
                    tempY_3D = actualDistance;
                    actualX = (tempX_3D * cos(angleRadian)) + (tempY_3D * -sin(angleRadian));
                    actualDistance = (tempX_3D * sin(angleRadian)) + (tempY_3D * cos(angleRadian));
                    
                    scan_3D.get()->points[index_3D].x = actualX * DIVISOR;
                    scan_3D.get()->points[index_3D].y = actualDistance * DIVISOR;
                    scan_3D.get()->points[index_3D].z = (actualY + HALF_ANGLE) * DIVISOR;
                }
                else
                {
                    scan_3D.get()->points[index_3D].a = 0;
                }
            }
        }

        drawing = false;
    }
}

void running()
{
    ros::NodeHandle nh;
    ros::NodeHandle priv_nh("~");

    std::string port;
    int baud_rate;
    std::string frame_id;

    priv_nh.param("port", port, std::string("/dev/ttyUSB0"));
    priv_nh.param("baud_rate", baud_rate, 3000000);
    priv_nh.param("version_num", version_num, 0);
    priv_nh.param("frame_id", frame_id, std::string("laser_link"));

    colorBuffer();

    pub = nh.advertise<sensor_msgs::PointCloud2>("scan", 1);
    scan_2D = pcl::PointCloud<pcl::PointXYZRGBA>::Ptr(new pcl::PointCloud<pcl::PointXYZRGBA>);
    scan_3D = pcl::PointCloud<pcl::PointXYZRGBA>::Ptr(new pcl::PointCloud<pcl::PointXYZRGBA>);
    
    // PointCloud2 for 2D
    scan_2D.get()->is_dense = false;
    scan_2D.get()->header.frame_id = frame_id;

    scan_2D.get()->points.resize(DATABUFFER_SIZE_2D);
    for (int point = 0; point < DATABUFFER_SIZE_2D; point++)
    {
        scan_2D.get()->points[point].a = 0;
    }

    // PointCloud2 for 3D
    scan_3D.get()->width = 160;
    scan_3D.get()->height = 60;
    scan_3D.get()->is_dense = false;
    scan_3D.get()->header.frame_id = frame_id;

    width = (float)(scan_3D.get()->width);
    height = (float)(scan_3D.get()->height);
    
    centerX_3D = (width / 2);
    centerY_3D = (height / 2);

    scan_3D.get()->points.resize(DATABUFFER_SIZE_3D);
    for (int point = 0; point < DATABUFFER_SIZE_3D; point++)
    {
        scan_3D.get()->points[point].a = 0;
    }

    actualDistance = 0.0;
    length = 0.0;
    currentBuffer = 0x00;
    drawing = false;

    boost::asio::io_service io;
    try
    {
        cyglidar_pcl_driver::cyglidar_pcl laser(port, baud_rate, io);
        laser.packet(version_num);

        while (ros::ok())
        {
            uint8_t* result = laser.poll(version_num);
            if (result[0] == 0x5A && result[1] == 0x77 && result[2] == 0xFF)
            {
                switch (result[5])
                {
                    case 0x01: // 2D
                        bufferPtr_2D = &result[0];
                        cloudScatter_2D();

                        pcl_conversions::toPCL(ros::Time::now(), scan_2D->header.stamp);
                        pub.publish(scan_2D);
                        break;
                    case 0x08: // 3D
                        switch (currentBuffer)
                        {
                            case 0x00:
                                bufferPtr01 = &result[0];
                                currentBuffer = 0x01;
                                break;
                            case 0x01:
                                bufferPtr02 = &result[0];
                                currentBuffer = 0x00;
                                break;
                        }
                        cloudScatter_3D();

                        pcl_conversions::toPCL(ros::Time::now(), scan_3D->header.stamp);
                        pub.publish(scan_3D);
                        break;
                }
            }
        }
        laser.close();
    }
    catch (boost::system::system_error ex)
    {
        ROS_ERROR("[Error] instantiating laser object. \
        Are you sure you have the correct port and baud rate? \
        Error was %s", ex.what());
    }
}

int main(int argc, char **argv)
{
    // Initialize ROS
    ros::init(argc, argv, "line_laser");

    std::thread first(running);
    first.join();

    return 0;
}
