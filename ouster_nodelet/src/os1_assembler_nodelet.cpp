// this should really be in the implementation (.cpp file)
#include <pluginlib/class_list_macros.h>

// Include your header
#include "ouster_nodelet/os1_assembler_nodelet.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <pcl_conversions/pcl_conversions.h>
#include <ros/console.h>
#include <ros/ros.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>

#include <ouster/os1_packet.h>
#include <ouster/os1_util.h>
#include <ouster_ros/OS1ConfigSrv.h>
#include <ouster_ros/PacketMsg.h>
#include <ouster_ros/os1_ros.h>

using ns = std::chrono::nanoseconds;
using PacketMsg = ouster_ros::PacketMsg;
using OS1ConfigSrv = ouster_ros::OS1ConfigSrv;
namespace OS1 = ouster::OS1;

namespace ouster_nodelet

{
void OS1AssemblerNodelet::onInit(){
    
    NODELET_INFO("[OS1 Assembler Nodelet] Initializing nodlet");
    nh = this->getMTPrivateNodeHandle();

    auto success = run();
}

int OS1AssemblerNodelet::run()
{

    OS1::sensor_info info{};
    auto srv =
        nh.advertiseService<OS1ConfigSrv::Request, OS1ConfigSrv::Response>(
            "os1_config",
            [&](OS1ConfigSrv::Request &, OS1ConfigSrv::Response &res) {
                res.hostname = info.hostname;
                res.lidar_mode = to_string(info.mode);
                res.beam_azimuth_angles = info.beam_azimuth_angles;
                res.beam_altitude_angles = info.beam_altitude_angles;
                res.imu_to_sensor_transform = info.imu_to_sensor_transform;
                res.lidar_to_sensor_transform = info.lidar_to_sensor_transform;
                return true ;
            });

    // empty indicates "not set" since roslaunch xml can't optionally set params
    auto hostname = nh.param("os1_hostname", std::string{});
    auto udp_dest = nh.param("os1_udp_dest", std::string{});
    auto lidar_port = nh.param("os1_lidar_port", 7501);
    auto imu_port = nh.param("os1_imu_port", 7502);
    auto replay = nh.param("replay", false);
    auto lidar_mode = nh.param("lidar_mode", std::string{});
    auto scan_dur = ns(nh.param("scan_dur_ns", 100000000));

    // fall back to metadata file name based on hostname, if available
    auto meta_file = nh.param("metadata", std::string{});
    if (!meta_file.size() && hostname.size())
        meta_file = hostname + ".json";

    if (lidar_mode.size())
    {
        if (replay)
            NODELET_WARN("Lidar mode set in replay mode. May be ignored");
    }
    else
    {
        lidar_mode = OS1::to_string(OS1::lidar_mode::MODE_1024x10);
    }

    if (OS1::lidar_mode_of_string(lidar_mode) == OS1::lidar_mode::MODE_INVALID)
    {
        NODELET_ERROR("Invalid lidar mode %s", lidar_mode.c_str());
        return EXIT_FAILURE;
    }

    if (!replay && (!hostname.size() || !udp_dest.size()))
    {
        NODELET_ERROR("Must specify both hostname and udp destination");
        return EXIT_FAILURE;
    }

    if (replay)
    {
        NODELET_INFO("Running in replay mode");

        // populate info for config service
        std::string metadata = read_metadata(meta_file);
        info = OS1::parse_metadata(metadata);
        populate_metadata_defaults(info, lidar_mode);

        NODELET_INFO("Using lidar_mode: %s", OS1::to_string(info.mode).c_str());
        NODELET_INFO("Sensor sn: %s firmware rev: %s", info.sn.c_str(),
                 info.fw_rev.c_str());

        // just serve config service
        ros::spin();
        return EXIT_SUCCESS;
    }
    else
    {
        NODELET_INFO("Connecting to sensor at %s...", hostname.c_str());

        NODELET_INFO("Sending data to %s using lidar_mode: %s", udp_dest.c_str(),
                 lidar_mode.c_str());

        auto cli = OS1::init_client(hostname, udp_dest,
                                    OS1::lidar_mode_of_string(lidar_mode),
                                    lidar_port, imu_port);

        if (!cli)
        {
            NODELET_ERROR("Failed to initialize sensor at: %s", hostname.c_str());
            return EXIT_FAILURE;
        }
        NODELET_INFO("Sensor reconfigured successfully, waiting for data...");

        // write metadata file to cwd (usually ~/.ros)
        auto metadata = OS1::get_metadata(*cli);
        write_metadata(meta_file, metadata);

        // populate sensor info
        info = OS1::parse_metadata(metadata);
        populate_metadata_defaults(info, "");

        NODELET_INFO("Sensor sn: %s firmware rev: %s", info.sn.c_str(),
                 info.fw_rev.c_str());

        // publish packet messages from the sensor
        return connection_loop(nh, *cli);
    }
};

void OS1AssemblerNodelet::populate_metadata_defaults(OS1::sensor_info &info,
                                                     const std::string &specified_lidar_mode)
{
    if (!info.hostname.size())
        info.hostname = "UNKNOWN";

    if (!info.sn.size())
        info.sn = "UNKNOWN";

    OS1::version v = OS1::version_of_string(info.fw_rev);
    if (v == OS1::invalid_version)
        NODELET_WARN("Unknown sensor firmware version; output may not be reliable");
    else if (v < OS1::min_version)
        NODELET_WARN("Firmware < %s not supported; output may not be reliable",
                 to_string(OS1::min_version).c_str());

    if (info.mode == OS1::lidar_mode::MODE_INVALID)
    {
        NODELET_WARN(
            "Lidar mode not found in metadata; output may not be reliable");
        info.mode = OS1::lidar_mode_of_string(specified_lidar_mode);
    }

    if (info.beam_azimuth_angles.empty() || info.beam_altitude_angles.empty())
    {
        NODELET_WARN("Beam angles not found in metadata; using design values");
        info.beam_azimuth_angles = OS1::beam_azimuth_angles;
        info.beam_altitude_angles = OS1::beam_altitude_angles;
    }

    if (info.imu_to_sensor_transform.empty() ||
        info.lidar_to_sensor_transform.empty())
    {
        NODELET_WARN("Frame transforms not found in metadata; using design values");
        info.imu_to_sensor_transform = OS1::imu_to_sensor_transform;
        info.lidar_to_sensor_transform = OS1::lidar_to_sensor_transform;
    }
};

// try to read metadata file
std::string OS1AssemblerNodelet::read_metadata(const std::string &meta_file)
{
    if (meta_file.size())
    {
        NODELET_INFO("Reading metadata from %s", meta_file.c_str());
    }
    else
    {
        NODELET_WARN("No metadata file specified");
        return "";
    }

    std::stringstream buf{};
    std::ifstream ifs{};
    ifs.open(meta_file);
    buf << ifs.rdbuf();
    ifs.close();

    if (!ifs)
        NODELET_WARN("Failed to read %s; check that the path is valid",
                 meta_file.c_str());

    return buf.str();
}

// try to write metadata file
void OS1AssemblerNodelet::write_metadata(const std::string &meta_file, const std::string &metadata)
{
    std::ofstream ofs;
    ofs.open(meta_file);
    ofs << metadata << std::endl;
    ofs.close();
    if (ofs)
    {
        NODELET_INFO("Wrote metadata to %s", meta_file.c_str());
    }
    else
    {
        NODELET_WARN("Failed to write metadata to %s; check that the path is valid",
                 meta_file.c_str());
    }
};

int OS1AssemblerNodelet::connection_loop(ros::NodeHandle &nh, OS1::client &cli)
{
    auto lidar_packet_pub = nh.advertise<PacketMsg>("lidar_packets", 1280);
    auto imu_packet_pub = nh.advertise<PacketMsg>("imu_packets", 100);

    PacketMsg lidar_packet, imu_packet;
    lidar_packet.buf.resize(OS1::lidar_packet_bytes + 1);
    imu_packet.buf.resize(OS1::imu_packet_bytes + 1);

    while (ros::ok())
    {
        auto state = OS1::poll_client(cli);
        if (state == OS1::EXIT)
        {
            NODELET_INFO("poll_client: caught signal, exiting");
            return EXIT_SUCCESS;
        }
        if (state & OS1::ERROR)
        {
            NODELET_ERROR("poll_client: returned error");
            return EXIT_FAILURE;
        }
        if (state & OS1::LIDAR_DATA)
        {
            if (OS1::read_lidar_packet(cli, lidar_packet.buf.data()))
                lidar_packet_pub.publish(lidar_packet);
        }
        if (state & OS1::IMU_DATA)
        {
            if (OS1::read_imu_packet(cli, imu_packet.buf.data()))
                imu_packet_pub.publish(imu_packet);
        }
        ros::spinOnce();
    }
    return EXIT_SUCCESS;
};

bool OS1AssemblerNodelet::validTimestamp(const ros::Time &msg_time)
{
    const ros::Duration kMaxTimeOffset(1.0);

    const ros::Time now = ros::Time::now();
    if (msg_time < (now - kMaxTimeOffset))
    {
        NODELET_WARN_STREAM_THROTTLE(
            1, "OS1 clock is currently not in sync with host. Current host time: "
                   << now << " OS1 message time: " << msg_time
                   << ". Rejecting measurement.");
        return false;
    }

    return true;
};

} // namespace ouster_nodelet

PLUGINLIB_EXPORT_CLASS(ouster_nodelet::OS1AssemblerNodelet, nodelet::Nodelet)