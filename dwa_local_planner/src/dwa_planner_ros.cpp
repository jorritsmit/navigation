/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2009, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
*********************************************************************/

#include <dwa_local_planner/dwa_planner_ros.h>
#include <Eigen/Core>
#include <cmath>

#include <ros/console.h>

#include <pluginlib/class_list_macros.h>

#include <base_local_planner/goal_functions.h>
#include <nav_msgs/Path.h>

//register this planner as a BaseLocalPlanner plugin
PLUGINLIB_EXPORT_CLASS(dwa_local_planner::DWAPlannerROS, nav_core::BaseLocalPlanner)

namespace dwa_local_planner {

    void setCmdVel(const base_local_planner::Trajectory& traj, geometry_msgs::Twist& c)
    {
        if (traj.cost_ >= 0)
        {
            c.linear.x = traj.xv_;
            c.linear.y = traj.yv_;
            c.angular.z = traj.thetav_;
        }
        else
        {
            ROS_WARN_STREAM("DWA PLANNER DISCARDED ALL TRAJECTORIES, COST: " << traj.cost_);
            c.linear.x = c.linear.y = c.angular.z = 0;
        }
    }

    void DWAPlannerROS::publishTrajectory(const base_local_planner::Trajectory& traj)
    {
        std::vector<geometry_msgs::PoseStamped> local_plan;
        if (traj.cost_ >= 0)
        {
            // Fill out the local plan
            for (unsigned int i = 0; i < traj.getPointsSize(); ++i)
            {
                double p_x, p_y, p_th;
                traj.getPoint(i, p_x, p_y, p_th);
                tf::Stamped<tf::Pose> p = tf::Stamped<tf::Pose>(tf::Pose( tf::createQuaternionFromYaw(p_th), tf::Point(p_x, p_y, 0.0)), ros::Time::now(), costmap_ros_->getGlobalFrameID());
                geometry_msgs::PoseStamped pose;
                tf::poseStampedTFToMsg(p, pose);
                local_plan.push_back(pose);
            }
        }
        base_local_planner::publishPlan(local_plan, l_traj_pub_);
    }

    void DWAPlannerROS::reconfigureCB(DWAPlannerConfig &config, uint32_t level) {
        if (setup_ && config.restore_defaults)
        {
            config = default_config_;
            config.restore_defaults = false;
        }
        if (!setup_)
        {
            default_config_ = config;
            setup_ = true;
        }

        // update generic local planner params
        base_local_planner::LocalPlannerLimits limits;
        limits.max_trans_vel = config.max_trans_vel;
        limits.min_trans_vel = config.min_trans_vel;
        limits.max_vel_x = config.max_vel_x;
        limits.min_vel_x = config.min_vel_x;
        limits.max_vel_y = config.max_vel_y;
        limits.min_vel_y = config.min_vel_y;
        limits.max_rot_vel = config.max_rot_vel;
        limits.min_rot_vel = config.min_rot_vel;
        limits.acc_lim_x = config.acc_lim_x;
        limits.acc_lim_y = config.acc_lim_y;
        limits.acc_lim_theta = config.acc_lim_theta;
        limits.acc_limit_trans = config.acc_limit_trans;
        limits.xy_goal_tolerance = config.xy_goal_tolerance;
        limits.yaw_goal_tolerance = config.yaw_goal_tolerance;
        limits.trans_stopped_vel = config.trans_stopped_vel;
        limits.rot_stopped_vel = config.rot_stopped_vel;

        // We want to prune the plan that we send to the local planner
        limits.prune_plan = config.prune_plan;
        limits.lookahead_distance = limits.max_trans_vel * config.sim_time;

        planner_util_.reconfigureCB(limits, config.restore_defaults);

        // update dwa specific configuration
        dp_->reconfigure(config);
    }

    DWAPlannerROS::DWAPlannerROS() :
        initialized_(false),
        odom_helper_("odom"),
        setup_(false)
    {

    }

    DWAPlannerROS::~DWAPlannerROS()
    {
        //make sure to clean things up
        delete dsrv_;
    }

    bool DWAPlannerROS::getRobotState(tf::Stamped<tf::Pose>& robot_pose, tf::Stamped<tf::Pose>& robot_vel)
    {
        if (!initialized_)
        {
            ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
            return false;
        }

        if (!costmap_ros_->getRobotPose(robot_pose))
        {
            ROS_ERROR("Could not get robot pose");
            return false;
        }

        // Get the velocity of the robot
        odom_helper_.getRobotVel(robot_vel);
    }

    bool DWAPlannerROS::getRobotStateAndLocalPlan(tf::Stamped<tf::Pose>& robot_pose, tf::Stamped<tf::Pose>& robot_vel, std::vector<geometry_msgs::PoseStamped>& local_plan)
    {
        getRobotState(robot_pose, robot_vel);

        if (!planner_util_.getLocalPlan(robot_pose, local_plan))
        {
            ROS_ERROR("Could not get local plan");
            return false;
        }

        if (local_plan.empty())
        {
            ROS_WARN_NAMED("dwa_local_planner", "Received an empty transformed plan.");
            return false;
        }
    }

    void DWAPlannerROS::initialize(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros)
    {
        if (initialized_)
        {
            ROS_WARN("This planner has already been initialized, doing nothing.");
            return;
        }

        ros::NodeHandle private_nh("~/" + name);
        l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);
        l_traj_pub_ = private_nh.advertise<nav_msgs::Path>("local_traj", 1);
        costmap_ros_ = costmap_ros;

        // make sure to update the costmap we'll use for this cycle
        costmap_2d::Costmap2D* costmap = costmap_ros_->getCostmap();

        planner_util_.initialize(tf, costmap, costmap_ros_->getGlobalFrameID());

        //create the actual planner that we'll use.. it'll configure itself from the parameter server
        dp_ = boost::shared_ptr<DWAPlanner>(new DWAPlanner(name, &planner_util_));

        initialized_ = true;

        dsrv_ = new dynamic_reconfigure::Server<DWAPlannerConfig>(private_nh);
        dynamic_reconfigure::Server<DWAPlannerConfig>::CallbackType cb = boost::bind(&DWAPlannerROS::reconfigureCB, this, _1, _2);
        dsrv_->setCallback(cb);
    }

    bool DWAPlannerROS::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan)
    {
        if (!initialized_)
        {
            ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
            return false;
        }

        ROS_INFO("Got new plan");
        return planner_util_.setPlan(orig_global_plan);
    }

    bool DWAPlannerROS::isGoalReached()
    {
        tf::Stamped<tf::Pose> robot_pose, robot_vel;
        if (!getRobotState(robot_pose, robot_vel))
            return false;

        base_local_planner::LocalPlannerLimits l = planner_util_.getCurrentLimits();
        tf::Stamped<tf::Pose> goal_pose;
        if (!planner_util_.getGoal(goal_pose))
            return false;

        double xy_to_goal    = base_local_planner::getGoalPositionDistance(robot_pose, goal_pose.getOrigin().getX(), goal_pose.getOrigin().getY());
        double angle_to_goal = base_local_planner::getGoalOrientationAngleDifference(robot_pose, tf::getYaw(goal_pose.getRotation()));
        bool   stopped       = base_local_planner::stopped(robot_vel, l.rot_stopped_vel, l.trans_stopped_vel);

        if (xy_to_goal <= l.xy_goal_tolerance && fabs(angle_to_goal) <= l.yaw_goal_tolerance && stopped)
            return true;
        else
            return false;
    }

    bool DWAPlannerROS::computeVelocityCommands(geometry_msgs::Twist& cmd_vel)
    {
        std::vector<geometry_msgs::PoseStamped> local_plan;
        tf::Stamped<tf::Pose> robot_pose, robot_vel;

        if (!getRobotStateAndLocalPlan(robot_pose, robot_vel, local_plan))
            return false;

        // Publish the local plan
        base_local_planner::publishPlan(local_plan, l_plan_pub_);

        // update plan in dwa planner to calculate cost grid
        dp_->updatePlanAndLocalCosts(robot_pose, local_plan, costmap_ros_->getRobotFootprint());

        // call with updated footprint
        base_local_planner::Trajectory traj = dp_->findBestPath(robot_pose, robot_vel);

        // Set the command velocity
        setCmdVel(traj, cmd_vel);

        // Publish the local plan
        publishTrajectory(traj);

        return traj.cost_ >= 0;
    }
}
