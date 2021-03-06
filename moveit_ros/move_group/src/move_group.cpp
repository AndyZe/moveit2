/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
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
 *   * Neither the name of Willow Garage nor the names of its
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
 *********************************************************************/

/* Author: Ioan Sucan */

#include <moveit/planning_scene_monitor/planning_scene_monitor.h>
#include <tf2_ros/transform_listener.h>
#include <moveit/move_group/move_group_capability.h>
#include <moveit/trajectory_execution_manager/trajectory_execution_manager.h>
#include <boost/tokenizer.hpp>
#include <moveit/macros/console_colors.h>
#include <moveit/move_group/node_name.h>
#include <memory>
#include <set>

static const std::string ROBOT_DESCRIPTION =
    "robot_description";  // name of the robot description (a param name, so it can be changed externally)

static const rclcpp::Logger LOGGER = rclcpp::get_logger("move_group.move_group");

namespace move_group
{
// These capabilities are loaded unless listed in disable_capabilities
// clang-format off
static const char* DEFAULT_CAPABILITIES[] = {
   "move_group/MoveGroupCartesianPathService",
   "move_group/MoveGroupKinematicsService",
   "move_group/MoveGroupExecuteTrajectoryAction",
   "move_group/MoveGroupMoveAction",
    // TODO (ddengster) : wait for port for moveit_ros_manipulation package
   //"move_group/MoveGroupPickPlaceAction",
   "move_group/MoveGroupPlanService",
   "move_group/MoveGroupQueryPlannersService",
   "move_group/MoveGroupStateValidationService",
   "move_group/MoveGroupGetPlanningSceneService",
   "move_group/ApplyPlanningSceneService",
   "move_group/ClearOctomapService",
};
// clang-format on

class MoveGroupExe
{
public:
  MoveGroupExe(const rclcpp::Node::SharedPtr& n, planning_scene_monitor::PlanningSceneMonitorPtr& psm, bool debug)
    : node_(n)
  {
    // if the user wants to be able to disable execution of paths, they can just set this ROS param to false
    bool allow_trajectory_execution;
    node_->get_parameter_or("allow_trajectory_execution", allow_trajectory_execution, true);

    context_.reset(new MoveGroupContext(node_, psm, allow_trajectory_execution, debug));

    // start the capabilities
    configureCapabilities();
  }

  ~MoveGroupExe()
  {
    capabilities_.clear();
    context_.reset();
    capability_plugin_loader_.reset();
  }

  void status()
  {
    if (context_)
    {
      if (context_->status())
      {
        if (capabilities_.empty())
          printf(MOVEIT_CONSOLE_COLOR_BLUE "\nmove_group is running but no capabilities are "
                                           "loaded.\n\n" MOVEIT_CONSOLE_COLOR_RESET);
        else
          printf(MOVEIT_CONSOLE_COLOR_GREEN "\nYou can start planning now!\n\n" MOVEIT_CONSOLE_COLOR_RESET);
        fflush(stdout);
      }
    }
    else
      RCLCPP_ERROR(LOGGER, "No MoveGroup context created. Nothing will work.");
  }

  MoveGroupContextPtr getContext()
  {
    return context_;
  }

private:
  void configureCapabilities()
  {
    try
    {
      capability_plugin_loader_.reset(
          new pluginlib::ClassLoader<MoveGroupCapability>("moveit_ros_move_group", "move_group::MoveGroupCapability"));
    }
    catch (pluginlib::PluginlibException& ex)
    {
      RCLCPP_FATAL_STREAM(LOGGER, "Exception while creating plugin loader for move_group capabilities: " << ex.what());
      return;
    }

    std::set<std::string> capabilities;

    // add default capabilities
    for (const char* capability : DEFAULT_CAPABILITIES)
      capabilities.insert(capability);

    // add capabilities listed in ROS parameter
    std::string capability_plugins;
    if (node_->get_parameter("capabilities", capability_plugins))
    {
      boost::char_separator<char> sep(" ");
      boost::tokenizer<boost::char_separator<char> > tok(capability_plugins, sep);
      capabilities.insert(tok.begin(), tok.end());
    }

    // drop capabilities that have been explicitly disabled
    if (node_->get_parameter("disable_capabilities", capability_plugins))
    {
      boost::char_separator<char> sep(" ");
      boost::tokenizer<boost::char_separator<char> > tok(capability_plugins, sep);
      for (boost::tokenizer<boost::char_separator<char> >::iterator cap_name = tok.begin(); cap_name != tok.end();
           ++cap_name)
        capabilities.erase(*cap_name);
    }

    for (const std::string& capability : capabilities)
    {
      try
      {
        printf(MOVEIT_CONSOLE_COLOR_CYAN "Loading '%s'...\n" MOVEIT_CONSOLE_COLOR_RESET, capability.c_str());
        MoveGroupCapabilityPtr cap = capability_plugin_loader_->createUniqueInstance(capability);
        cap->setContext(context_);
        cap->initialize();
        capabilities_.push_back(cap);
      }
      catch (pluginlib::PluginlibException& ex)
      {
        RCLCPP_ERROR_STREAM(LOGGER, "Exception while loading move_group capability '" << capability
                                                                                      << "': " << ex.what());
      }
    }

    std::stringstream ss;
    ss << std::endl;
    ss << std::endl;
    ss << "********************************************************" << std::endl;
    ss << "* MoveGroup using: " << std::endl;
    for (const MoveGroupCapabilityPtr& cap : capabilities_)
      ss << "*     - " << cap->getName() << std::endl;
    ss << "********************************************************" << std::endl;
    RCLCPP_INFO(LOGGER, "%s", ss.str().c_str());
  }

  rclcpp::Node::SharedPtr node_;
  MoveGroupContextPtr context_;
  std::shared_ptr<pluginlib::ClassLoader<MoveGroupCapability> > capability_plugin_loader_;
  std::vector<MoveGroupCapabilityPtr> capabilities_;
};
}  // namespace move_group

template <typename T>
T getParameterFromRemoteNode(const rclcpp::Node::SharedPtr& node, const std::string& node_name,
                             const std::string& param_name)
{
  using namespace std::chrono_literals;
  auto parameters_client = std::make_shared<rclcpp::SyncParametersClient>(node, node_name);
  while (!parameters_client->wait_for_service(0.5s))
  {
    if (!rclcpp::ok())
    {
      RCLCPP_ERROR(LOGGER, "Interrupted while waiting for the service. Exiting.");
      return T();
    }
    RCLCPP_INFO(LOGGER, "service not available, waiting again...");
  }

  T param_value = parameters_client->get_parameter<T>(param_name, T());
  return param_value;
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);

  rclcpp::NodeOptions opt;
  opt.allow_undeclared_parameters(true);
  opt.automatically_declare_parameters_from_overrides(true);
  rclcpp::Node::SharedPtr nh = rclcpp::Node::make_shared("move_group", opt);

  // fetch a bunch of parameters
  {
    std::string robot_desc_param = "robot_description";
    std::string str = getParameterFromRemoteNode<std::string>(nh, "robot_state_publisher", robot_desc_param);
    nh->declare_parameter(robot_desc_param);
    nh->set_parameter(rclcpp::Parameter(robot_desc_param, str));

    std::string semantic_file = nh->get_parameter("robot_description_semantic").as_string();
    std::ifstream file(semantic_file);
    std::stringstream buffer;
    buffer << file.rdbuf();
    nh->set_parameter(rclcpp::Parameter("robot_description_semantic", buffer.str()));
  }

  std::shared_ptr<tf2_ros::Buffer> tf_buffer =
      std::make_shared<tf2_ros::Buffer>(nh->get_clock(), tf2::durationFromSec(10.0));
  std::shared_ptr<tf2_ros::TransformListener> tfl = std::make_shared<tf2_ros::TransformListener>(*tf_buffer);

  planning_scene_monitor::PlanningSceneMonitorPtr planning_scene_monitor(
      new planning_scene_monitor::PlanningSceneMonitor(nh, ROBOT_DESCRIPTION, tf_buffer));

  if (planning_scene_monitor->getPlanningScene())
  {
    bool debug = false;
    for (int i = 1; i < argc; ++i)
      if (strncmp(argv[i], "--debug", 7) == 0)
      {
        debug = true;
        break;
      }
    debug = true;
    if (debug)
      RCLCPP_INFO(LOGGER, "MoveGroup debug mode is ON");
    else
      RCLCPP_INFO(LOGGER, "MoveGroup debug mode is OFF");

    rclcpp::executors::MultiThreadedExecutor executor;
    rclcpp::Node::SharedPtr monitor_node = rclcpp::Node::make_shared("monitor_node", opt);

    printf(MOVEIT_CONSOLE_COLOR_CYAN "Starting planning scene monitors...\n" MOVEIT_CONSOLE_COLOR_RESET);
    planning_scene_monitor->startSceneMonitor();
    planning_scene_monitor->startWorldGeometryMonitor();
    planning_scene_monitor->startStateMonitor();
    printf(MOVEIT_CONSOLE_COLOR_CYAN "Planning scene monitors started.\n" MOVEIT_CONSOLE_COLOR_RESET);

    move_group::MoveGroupExe mge(nh, planning_scene_monitor, debug);

    planning_scene_monitor->publishDebugInformation(debug);

    mge.status();
    auto controller_mgr_node = mge.getContext()->trajectory_execution_manager_->getControllerManagerNode();
    executor.add_node(controller_mgr_node);
    executor.add_node(monitor_node);
    executor.add_node(nh);
    executor.spin();

    rclcpp::shutdown();
  }
  else
    RCLCPP_ERROR(LOGGER, "Planning scene not configured");

  return 0;
}