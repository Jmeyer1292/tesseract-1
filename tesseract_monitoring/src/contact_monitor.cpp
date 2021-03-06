#include <pluginlib/class_loader.hpp>
#include <ros/ros.h>
#include <sensor_msgs/JointState.h>
#include <srdfdom/model.h>
#include <tesseract_core/discrete_contact_manager_base.h>
#include <tesseract_msgs/ContactResultVector.h>
#include <tesseract_msgs/ModifyTesseractEnv.h>
#include <tesseract_ros/kdl/kdl_env.h>
#include <tesseract_ros/ros_tesseract_utils.h>
#include <urdf_parser/urdf_parser.h>

using namespace tesseract;
using namespace tesseract::tesseract_ros;

const std::string ROBOT_DESCRIPTION_PARAM = "robot_description"; /**< Default ROS parameter for robot description */

const double DEFAULT_CONTACT_DISTANCE = 0.1;

KDLEnvPtr env;
DiscreteContactManagerBasePtr manager;
ros::Subscriber joint_states_sub;
ros::Publisher contact_results_pub;
ros::Publisher environment_pub;
ros::ServiceServer modify_env_service;
ContactTestType type;
ContactResultMap contacts;
tesseract_msgs::ContactResultVector contacts_msg;
bool publish_environment;
boost::mutex modify_mutex;

void callbackJointState(const sensor_msgs::JointState::ConstPtr& msg)
{
  boost::mutex::scoped_lock(modify_mutex);
  contacts.clear();
  contacts_msg.constacts.clear();

  env->setState(msg->name, msg->position);
  EnvStateConstPtr state = env->getState();

  manager->setCollisionObjectsTransform(state->transforms);
  manager->contactTest(contacts, type);

  if (publish_environment)
  {
    tesseract_msgs::TesseractState state_msg;
    tesseract_ros::tesseractToTesseractStateMsg(state_msg, *env);
    environment_pub.publish(state_msg);
  }

  ContactResultVector contacts_vector;
  tesseract::moveContactResultsMapToContactResultsVector(contacts, contacts_vector);
  contacts_msg.constacts.reserve(contacts_vector.size());
  for (const auto& contact : contacts_vector)
  {
    tesseract_msgs::ContactResult contact_msg;
    tesseractContactResultToContactResultMsg(contact_msg, contact, msg->header.stamp);
    contacts_msg.constacts.push_back(contact_msg);
  }
  contact_results_pub.publish(contacts_msg);
}

bool callbackModifyTesseractEnv(tesseract_msgs::ModifyTesseractEnvRequest& request,
                                tesseract_msgs::ModifyTesseractEnvResponse& response)
{
  boost::mutex::scoped_lock(modify_mutex);
  response.success = processTesseractStateMsg(*env, request.state);

  // Create a new manager
  std::vector<std::string> active = manager->getActiveCollisionObjects();
  double contact_distance = manager->getContactDistanceThreshold();
  IsContactAllowedFn fn = manager->getIsContactAllowedFn();

  manager = env->getDiscreteContactManager();
  manager->setActiveCollisionObjects(active);
  manager->setContactDistanceThreshold(contact_distance);
  manager->setIsContactAllowedFn(fn);

  return true;
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "tesseract_contact_monitoring");
  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");
  urdf::ModelInterfaceSharedPtr urdf_model;
  srdf::ModelSharedPtr srdf_model;
  std::string robot_description;
  std::string plugin;

  env.reset(new KDLEnv());

  pnh.param<std::string>("robot_description", robot_description, ROBOT_DESCRIPTION_PARAM);
  pnh.param<bool>("publish_environment", publish_environment, false);
  if (pnh.hasParam("plugin"))
  {
    pnh.getParam("plugin", plugin);
    env->loadDiscreteContactManagerPlugin(plugin);
  }

  // Initial setup
  std::string urdf_xml_string, srdf_xml_string;
  if (!nh.hasParam(robot_description))
  {
    ROS_ERROR("Failed to find parameter: %s", robot_description.c_str());
    return -1;
  }

  if (!nh.hasParam(robot_description + "_semantic"))
  {
    ROS_ERROR("Failed to find parameter: %s", (robot_description + "_semantic").c_str());
    return -1;
  }

  nh.getParam(robot_description, urdf_xml_string);
  nh.getParam(robot_description + "_semantic", srdf_xml_string);

  urdf_model = urdf::parseURDF(urdf_xml_string);
  srdf_model = srdf::ModelSharedPtr(new srdf::Model);
  srdf_model->initString(*urdf_model, srdf_xml_string);
  if (urdf_model == nullptr)
  {
    ROS_ERROR("Failed to parse URDF.");
    return -1;
  }

  if (!env->init(urdf_model, srdf_model))
  {
    ROS_ERROR("Failed to initialize environment.");
    return -1;
  }

  // Setup request information
  std::vector<std::string> link_names;
  double contact_distance;

  pnh.param<double>("contact_distance", contact_distance, DEFAULT_CONTACT_DISTANCE);

  link_names = env->getLinkNames();
  if (pnh.hasParam("monitor_links"))
    pnh.getParam("monitor_links", link_names);

  if (link_names.empty())
    link_names = env->getLinkNames();

  int contact_test_type = 2;
  if (pnh.hasParam("contact_test_type"))
    pnh.getParam("contact_test_type", contact_test_type);

  if (contact_test_type < 0 || contact_test_type > 3)
  {
    ROS_WARN("Request type must be 0, 1, 2 or 3. Setting to 2(ALL)!");
    contact_test_type = 2;
  }
  type = static_cast<ContactTestType>(contact_test_type);

  manager = env->getDiscreteContactManager();
  manager->setActiveCollisionObjects(link_names);
  manager->setContactDistanceThreshold(contact_distance);

  joint_states_sub = nh.subscribe("joint_states", 1, &callbackJointState);
  contact_results_pub = pnh.advertise<tesseract_msgs::ContactResultVector>("contact_results", 1, true);
  modify_env_service =
      pnh.advertiseService<tesseract_msgs::ModifyTesseractEnvRequest, tesseract_msgs::ModifyTesseractEnvResponse>(
          "modify_tesseract_env", &callbackModifyTesseractEnv);

  if (publish_environment)
    environment_pub = pnh.advertise<tesseract_msgs::TesseractState>("tesseract", 100, false);

  ROS_INFO("Contact Monitor Running!");

  ros::spin();

  return 0;
}
