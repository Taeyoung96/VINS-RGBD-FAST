#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CompressedImage.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/Bool.h>
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>

#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include "feature_tracker.h"

#define SHOW_UNDISTORTION 0

vector<uchar> r_status;
vector<float> r_err;
queue<sensor_msgs::ImageConstPtr> img_buf;

ros::Publisher pub_img, pub_match;
ros::Publisher pub_restart;

FeatureTracker trackerData[NUM_OF_CAM];
double first_image_time;
int pub_count = 1;
bool first_image_flag = true;
double last_image_time = 0;
double prev_image_time = 0;
bool init_pub = 0;

std::vector<sensor_msgs::Imu> imu_msg_buffer;

void imu_callback(
        const sensor_msgs::ImuConstPtr &msg) {
    // Wait for the first image to be set.
    if (first_image_flag) return;
    imu_msg_buffer.push_back(*msg);
}

Matrix3d integrateImuData() {
    if (imu_msg_buffer.empty()) {
        std::cout<<"empty IMU!"<<std::endl;
        return Matrix3d::Identity();
    }
    // Find the start and the end limit within the imu msg buffer.
    auto begin_iter = imu_msg_buffer.begin();
    while (begin_iter != imu_msg_buffer.end()) {
        if ((begin_iter->header.stamp.toSec() - last_image_time) < -0.01)
            ++begin_iter;
        else
            break;
    }

    auto end_iter = begin_iter;
    while (end_iter != imu_msg_buffer.end()) {
        if ((end_iter->header.stamp.toSec() - last_image_time) < 0.005)
            ++end_iter;
        else
            break;
    }

    // Compute the mean angular velocity in the IMU frame.
    Vector3d mean_ang_vel(0.0, 0.0, 0.0);
    for (auto iter = begin_iter; iter < end_iter; ++iter)
        mean_ang_vel += Vector3d(iter->angular_velocity.x,
                                 iter->angular_velocity.y, iter->angular_velocity.z);

     if (end_iter - begin_iter > 0)
        mean_ang_vel *= 1.0f / (end_iter - begin_iter);

    // Transform the mean angular velocity from the IMU
    // frame to the cam0 frames.
    Vector3d cam0_mean_ang_vel = Ric.transpose() * mean_ang_vel;

    // Compute the relative rotation.
    double dtime = last_image_time - prev_image_time;
    Vector3d cam0_angle_axisd = cam0_mean_ang_vel * dtime;

    // Delete the useless and used imu messages.
    imu_msg_buffer.erase(imu_msg_buffer.begin(), end_iter);
    return AngleAxisd(cam0_angle_axisd.norm(), cam0_angle_axisd.normalized()).toRotationMatrix().transpose();
}

void img_callback(const sensor_msgs::CompressedImageConstPtr &color_msg,
                  const sensor_msgs::CompressedImageConstPtr &depth_msg) {
    if (first_image_flag) {
        first_image_flag = false;
        first_image_time = color_msg->header.stamp.toSec();
        last_image_time = color_msg->header.stamp.toSec();
        return;
    }
    // detect unstable camera stream
    if (color_msg->header.stamp.toSec() - last_image_time > 1.0 || color_msg->header.stamp.toSec() < last_image_time) {
        ROS_WARN("image discontinue! reset the feature tracker!");
        first_image_flag = true;
        last_image_time = 0;
        pub_count = 1;
        std_msgs::Bool restart_flag;
        restart_flag.data = true;
        pub_restart.publish(restart_flag);
        return;
    }
    prev_image_time = last_image_time;
    last_image_time = color_msg->header.stamp.toSec();
    // frequency control
    if (round(1.0 * pub_count / (color_msg->header.stamp.toSec() - first_image_time)) <= FREQ) {
        PUB_THIS_FRAME = true;
        // reset the frequency control
        if (abs(1.0 * pub_count / (color_msg->header.stamp.toSec() - first_image_time) - FREQ) < 0.01 * FREQ) {
            first_image_time = color_msg->header.stamp.toSec();
            pub_count = 0;
        }
    } else
        PUB_THIS_FRAME = false;
    // encodings in ros: http://docs.ros.org/diamondback/api/sensor_msgs/html/image__encodings_8cpp_source.html
    //color has encoding RGB8
    cv_bridge::CvImageConstPtr ptr;
    ptr = cv_bridge::toCvCopy(color_msg, sensor_msgs::image_encodings::MONO8);

    // Get compressed image data
    // https://answers.ros.org/question/51490/sensor_msgscompressedimage-decompression/
    // https://github.com/heleidsn/heleidsn.github.io/blob/c02ed13cb4ffe85ee8f03a9ad93fa55336f84f7c/source/_posts/realsense-depth-image.md
    // https://sourcegraph.com/github.com/ros-perception/image_transport_plugins/-/blob/compressed_depth_image_transport/include/compressed_depth_image_transport/codec.h
    const std::vector<uint8_t> imageData(depth_msg->data.begin() + 12, depth_msg->data.end());
    cv::Mat depth_img = cv::imdecode(imageData, cv::IMREAD_UNCHANGED);

    cv::Mat show_img = ptr->image;
    TicToc t_r;
    for (int i = 0; i < NUM_OF_CAM; i++) {
        ROS_DEBUG("processing camera %d", i);
        if (i != 1 || !STEREO_TRACK) {
            Matrix3d relative_R = integrateImuData();
            trackerData[i].readImage(ptr->image.rowRange(ROW * i, ROW * (i + 1)),
                                     color_msg->header.stamp.toSec(),
                                     relative_R);
        } else {
            if (EQUALIZE) {
                cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
                clahe->apply(ptr->image.rowRange(ROW * i, ROW * (i + 1)), trackerData[i].cur_img);
            } else {
                trackerData[i].cur_img = ptr->image.rowRange(ROW * i, ROW * (i + 1));
            }

        }
//always 0
#if SHOW_UNDISTORTION
        trackerData[i].showUndistortion("undistrotion_" + std::to_string(i));
#endif
    }
    // update all id in ids[]
    //If has ids[i] == -1 (newly added pts by cv::goodFeaturesToTrack), substitute by gloabl id counter (n_id)
    for (unsigned int i = 0;; i++) {
        bool completed = false;
        for (int j = 0; j < NUM_OF_CAM; j++)
            if (j != 1 || !STEREO_TRACK)
                completed |= trackerData[j].updateID(i);
        if (!completed)
            break;
    }
    if (PUB_THIS_FRAME) {
        vector<int> test;
        pub_count++;
        //http://docs.ros.org/api/sensor_msgs/html/msg/PointCloud.html
        sensor_msgs::PointCloudPtr feature_points(new sensor_msgs::PointCloud);
        sensor_msgs::ChannelFloat32 id_of_point;
        sensor_msgs::ChannelFloat32 u_of_point;
        sensor_msgs::ChannelFloat32 v_of_point;
        sensor_msgs::ChannelFloat32 velocity_x_of_point;
        sensor_msgs::ChannelFloat32 velocity_y_of_point;
        //Use round to get depth value of corresponding points
        sensor_msgs::ChannelFloat32 depth_of_point;

        feature_points->header = color_msg->header;
        feature_points->header.frame_id = "world";

        for (int i = 0; i < NUM_OF_CAM; i++) {
            auto &un_pts = trackerData[i].cur_un_pts;
            auto &cur_pts = trackerData[i].cur_pts;
            auto &ids = trackerData[i].ids;
            auto &pts_velocity = trackerData[i].pts_velocity;
            for (unsigned int j = 0; j < ids.size(); j++) {
                if (trackerData[i].track_cnt[j] > 1) {
                    int p_id = ids[j];
                    geometry_msgs::Point32 p;
                    p.x = un_pts[j].x;
                    p.y = un_pts[j].y;
                    p.z = 1;
                    // push normalized point to pointcloud
                    feature_points->points.push_back(p);
                    // push other info
                    id_of_point.values.push_back(p_id * NUM_OF_CAM + i);
                    u_of_point.values.push_back(cur_pts[j].x);
                    v_of_point.values.push_back(cur_pts[j].y);
                    velocity_x_of_point.values.push_back(pts_velocity[j].x);
                    velocity_y_of_point.values.push_back(pts_velocity[j].y);

                    //nearest neighbor....fastest  may be changed
                    // show_depth: 480*640   y:[0,480]   x:[0,640]
                    depth_of_point.values.push_back(
                            (int) depth_img.at<unsigned short>(round(cur_pts[j].y), round(cur_pts[j].x)));
                }
            }
        }

        feature_points->channels.push_back(id_of_point);
        feature_points->channels.push_back(u_of_point);
        feature_points->channels.push_back(v_of_point);
        feature_points->channels.push_back(velocity_x_of_point);
        feature_points->channels.push_back(velocity_y_of_point);
        feature_points->channels.push_back(depth_of_point);
        ROS_DEBUG("publish %f, at %f", feature_points->header.stamp.toSec(), ros::Time::now().toSec());
        // skip the first image; since no optical speed on frist image
        if (!init_pub) {
            init_pub = 1;
        } else {
            pub_img.publish(feature_points);//"feature"
        }
        // Show image with tracked points in rviz (by topic pub_match)
        if (SHOW_TRACK) {
            ptr = cv_bridge::cvtColor(ptr, sensor_msgs::image_encodings::BGR8);
            //cv::Mat stereo_img(ROW * NUM_OF_CAM, COL, CV_8UC3);
            cv::Mat stereo_img = ptr->image;

            for (int i = 0; i < NUM_OF_CAM; i++) {
                cv::Mat tmp_img = stereo_img.rowRange(i * ROW, (i + 1) * ROW);
                cv::cvtColor(show_img, tmp_img, CV_GRAY2RGB);//??seems useless?

                for (unsigned int j = 0; j < trackerData[i].cur_pts.size(); j++) {
                    double len = std::min(1.0, 1.0 * trackerData[i].track_cnt[j] / WINDOW_SIZE);
                    cv::circle(tmp_img, trackerData[i].cur_pts[j], 2, cv::Scalar(255 * (1 - len), 0, 255 * len), 2);
                    //draw speed line
                    /*
                    Vector2d tmp_cur_un_pts (trackerData[i].cur_un_pts[j].x, trackerData[i].cur_un_pts[j].y);
                    Vector2d tmp_pts_velocity (trackerData[i].pts_velocity[j].x, trackerData[i].pts_velocity[j].y);
                    Vector3d tmp_prev_un_pts;
                    tmp_prev_un_pts.head(2) = tmp_cur_un_pts - 0.10 * tmp_pts_velocity;
                    tmp_prev_un_pts.z() = 1;
                    Vector2d tmp_prev_uv;
                    trackerData[i].m_camera->spaceToPlane(tmp_prev_un_pts, tmp_prev_uv);
                    cv::line(tmp_img, trackerData[i].cur_pts[j], cv::Point2f(tmp_prev_uv.x(), tmp_prev_uv.y()), cv::Scalar(255 , 0, 0), 1 , 8, 0);
                    */
                    //char name[10];
                    //sprintf(name, "%d", trackerData[i].ids[j]);
                    //cv::putText(tmp_img, name, trackerData[i].cur_pts[j], cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
                }
                for (unsigned int j = 0; j < trackerData[i].predict_pts.size(); j++) {
                    cv::circle(tmp_img, trackerData[i].predict_pts[j], 1, cv::Scalar(0, 255, 0), 1);
                }
            }

            pub_match.publish(ptr->toImageMsg());
        }
    }
//    ROS_INFO("whole feature tracker processing costs: %f", t_r.toc());
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "feature_tracker");
    ros::NodeHandle n("~");
    /**
     * rosrun rqt_logger_level rqt_logger_level
     * 将对应level设置为debug可看到终端输出ROS_DEBUG
     */
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);

    readParameters(n);

    //trackerData defined as global parameter   type: FeatureTracker list   size: 1
    for (int i = 0; i < NUM_OF_CAM; i++)
        trackerData[i].readIntrinsicParameter(CAM_NAMES[i]);

    if (FISHEYE) {
        for (int i = 0; i < NUM_OF_CAM; i++) {
            trackerData[i].fisheye_mask = cv::imread(FISHEYE_MASK, 0);
            if (!trackerData[i].fisheye_mask.data) {
                ROS_INFO("load mask fail");
                ROS_BREAK();
            } else
                ROS_INFO("load mask success");
        }
    }

    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 1000, imu_callback);

    //ref: http://docs.ros.org/api/message_filters/html/c++/classmessage__filters_1_1TimeSynchronizer.html#a9e58750270e40a2314dd91632a9570a6
    //     https://blog.csdn.net/zyh821351004/article/details/47758433
    message_filters::Subscriber<sensor_msgs::CompressedImage> sub_image(n, IMAGE_TOPIC + "/compressed", 100);
    message_filters::Subscriber<sensor_msgs::CompressedImage> sub_depth(n, DEPTH_TOPIC + "/compressedDepth", 100);

    // use ApproximateTime to fit fisheye camera
    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::CompressedImage, sensor_msgs::CompressedImage> syncPolicy;
    message_filters::Synchronizer<syncPolicy> sync(syncPolicy(100), sub_image, sub_depth);
    sync.registerCallback(boost::bind(&img_callback, _1, _2));

    //返回一个ros::Publisher对象  std_msgs::xxx的publisher,     1000: queue size
    pub_img = n.advertise<sensor_msgs::PointCloud>("feature", 100);
    pub_match = n.advertise<sensor_msgs::Image>("feature_img", 5);
    pub_restart = n.advertise<std_msgs::Bool>("restart", 5);

    ros::spin();
    return 0;
}


// new points velocity is 0, pub or not?
// track cnt > 1 pub?