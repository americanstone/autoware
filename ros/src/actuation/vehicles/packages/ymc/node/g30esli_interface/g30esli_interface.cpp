/*
 * Copyright 2017-2019 Autoware Foundation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cmath>
#include <thread>

#include <ros/ros.h>
#include <autoware_msgs/VehicleCmd.h>

#include "cansend.h"
#include "g30esli.h"

#include "g30esli_interface_util.h"

// ros subscriber
ros::Subscriber vehicle_cmd_sub_;

// ros publisher
ros::Publisher current_twist_pub_;

// ros param
std::string device_;
double steering_offset_deg_;

// variables
bool terminate_thread_;
bool engage_;
double target_velocity_kmph_;
double target_steering_angle_deg_;

// ymc g30esli tool
ymc::G30esli g30esli_;
ymc::G30esli::Status status_;
ymc::G30esli::Command command_;

void vehicle_cmd_callback(const autoware_msgs::VehicleCmdConstPtr& msg)
{
  // speed
  target_velocity_kmph_ = msg->ctrl_cmd.linear_velocity * 3.6;  // [m/s] -> [km/h]
  command_.speed = engage_ ? target_velocity_kmph_ : 0.0;

  // steer
  target_steering_angle_deg_ = msg->ctrl_cmd.steering_angle / M_PI * 180.0;  // [rad] -> [deg]
  target_steering_angle_deg_ = -target_steering_angle_deg_ + steering_offset_deg_;
  command_.steer = target_steering_angle_deg_;

  // mode
  command_.mode = engage_ ? G30ESLI_MODE_AUTO : G30ESLI_MODE_MANUAL;

  // brake
  command_.brake = (msg->emergency == 1) ? G30ESLI_BRAKE_EMERGENCY : G30ESLI_BRAKE_NONE;

  // shift
  if (msg->gear == 1)
  {
    command_.shift = G30ESLI_SHIFT_DRIVE;
  }
  else if (msg->gear == 2)
  {
    command_.shift = G30ESLI_SHIFT_REVERSE;
  }

  // flasher
  if (msg->lamp_cmd.l == 0 && msg->lamp_cmd.r == 0)
  {
    command_.flasher = G30ESLI_FLASHER_NONE;
  }
  else if (msg->lamp_cmd.l == 1 && msg->lamp_cmd.r == 0)
  {
    command_.flasher = G30ESLI_FLASHER_LEFT;
  }
  else if (msg->lamp_cmd.l == 0 && msg->lamp_cmd.r == 1)
  {
    command_.flasher = G30ESLI_FLASHER_RIGHT;
  }
  else if (msg->lamp_cmd.l == 1 && msg->lamp_cmd.r == 1)
  {
    command_.flasher = G30ESLI_FLASHER_HAZARD;
  }
}

// receive input from keyboard
// change the mode to manual mode or auto drive mode
void changeMode()
{
  while (!terminate_thread_)
  {
    if (kbhit())
    {
      char c = getchar();
      if (c == 's')
      {
        engage_ = true;
      }
      else if (c == ' ')
      {
        engage_ = false;
      }
    }
    usleep(1000);
  }
}

// read vehicle status
void readStatus()
{
  while (!terminate_thread_)
  {
    g30esli_.readStatus(status_);

    double lv = status_.speed.actual / 3.6;             // [km/h] -> [m/s]
    double th = -status_.steer.actual * M_PI / 180.0;   // [deg] -> [rad]
    double az = std::tan(th) * lv / G30ESLI_WHEEL_BASE; // [rad] -> [rad/s]

    // publish twist
    geometry_msgs::TwistStamped ts;
    ts.header.frame_id = "base_link";
    ts.header.stamp = ros::Time::now();
    ts.twist.linear.x = v;
    ts.twist.angular.z = w;
    current_twist_pub_.publish(ts);

    // accel/brake override, switch to manual mode
    engage_ = (status_.override.accel == 1 || status_.override.brake == 1) ? false : engage_;

    usleep(10);
  }
}

int main(int argc, char* argv[])
{
  // ros initialization
  ros::init(argc, argv, "g30esli_interface");
  ros::NodeHandle nh_;
  ros::NodeHandle private_nh("~");

  // rosparam
  private_nh.param<std::string>("device", device_, "can0");
  private_nh.param<double>("steering_offset_deg", steering_offset_deg_, 0.0);

  // subscriber
  vehicle_cmd_sub_ = nh_.subscribe<autoware_msgs::VehicleCmd>("vehicle_cmd", 1, vehicle_cmd_callback);

  // publisher
  current_twist_pub_ = nh_.advertise<geometry_msgs::TwistStamped>("ymc/twist", 10);

  // open can device
  if (!g30esli_.openDevice(device_))
  {
    std::cerr << "Cannot open device" << std::endl;
    return -1;
  }

  // create threads
  terminate_thread_ = false;
  std::thread t1(changeMode);
  std::thread t2(readStatus);

  // start by manual mode
  engage_ = false;

  ros::Rate rate(100);

  while (ros::ok())
  {
    // update subscribers
    ros::spinOnce();

    // send vehicle command
    g30esli_.sendCommand(command_);

    // update heart beat
    command_.alive++;

    // loop sleep
    rate.sleep();
  }

  terminate_thread_ = true;
  t1.join();
  t2.join();

  return 0;
}
