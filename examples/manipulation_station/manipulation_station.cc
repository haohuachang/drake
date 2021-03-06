#include "drake/examples/manipulation_station/manipulation_station.h"

#include <memory>
#include <string>
#include <utility>

#include "drake/common/find_resource.h"
#include "drake/geometry/dev/scene_graph.h"
#include "drake/manipulation/schunk_wsg/schunk_wsg_constants.h"
#include "drake/manipulation/schunk_wsg/schunk_wsg_position_controller.h"
#include "drake/math/rigid_transform.h"
#include "drake/math/rotation_matrix.h"
#include "drake/multibody/parsing/parser.h"
#include "drake/multibody/tree/prismatic_joint.h"
#include "drake/multibody/tree/revolute_joint.h"
#include "drake/multibody/tree/uniform_gravity_field_element.h"
#include "drake/systems/controllers/inverse_dynamics_controller.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/primitives/adder.h"
#include "drake/systems/primitives/constant_vector_source.h"
#include "drake/systems/primitives/demultiplexer.h"
#include "drake/systems/primitives/discrete_derivative.h"
#include "drake/systems/primitives/linear_system.h"
#include "drake/systems/primitives/matrix_gain.h"
#include "drake/systems/primitives/pass_through.h"
#include "drake/systems/sensors/dev/rgbd_camera.h"

namespace drake {
namespace examples {
namespace manipulation_station {

using Eigen::Isometry3d;
using Eigen::MatrixXd;
using Eigen::Vector3d;
using Eigen::VectorXd;
using geometry::SceneGraph;
using math::RigidTransform;
using math::RollPitchYaw;
using math::RotationMatrix;
using multibody::Joint;
using multibody::MultibodyPlant;
using multibody::PrismaticJoint;
using multibody::RevoluteJoint;
using multibody::SpatialInertia;

const int kNumDofIiwa = 7;

namespace internal {

// TODO(amcastro-tri): Refactor this into schunk_wsg directory, and cover it
// with a unit test.  Potentially tighten the tolerance in
// station_simulation_test.
// @param gripper_body_frame_name Name of a frame that's attached to the
// gripper's main body.
SpatialInertia<double> MakeCompositeGripperInertia(
    const std::string& wsg_sdf_path,
    const std::string& gripper_body_frame_name) {
  MultibodyPlant<double> plant;
  multibody::Parser parser(&plant);
  parser.AddModelFromFile(wsg_sdf_path);
  plant.Finalize();
  const auto& frame = plant.GetFrameByName(gripper_body_frame_name);
  const auto& gripper_body =
      plant.tree().GetRigidBodyByName(frame.body().name());
  const auto& left_finger = plant.tree().GetRigidBodyByName("left_finger");
  const auto& right_finger = plant.tree().GetRigidBodyByName("right_finger");
  const auto& left_slider = plant.GetJointByName("left_finger_sliding_joint");
  const auto& right_slider = plant.GetJointByName("right_finger_sliding_joint");
  const SpatialInertia<double>& M_GGo_G =
      gripper_body.default_spatial_inertia();
  const SpatialInertia<double>& M_LLo_L = left_finger.default_spatial_inertia();
  const SpatialInertia<double>& M_RRo_R =
      right_finger.default_spatial_inertia();
  auto CalcFingerPoseInGripperFrame = [](const Joint<double>& slider) {
    // Pose of the joint's parent frame P (attached on gripper body G) in the
    // frame of the gripper G.
    const RigidTransform<double> X_GP(
        slider.frame_on_parent().GetFixedPoseInBodyFrame());
    // Pose of the joint's child frame C (attached on the slider's finger body)
    // in the frame of the slider's finger F.
    const RigidTransform<double> X_FC(
        slider.frame_on_child().GetFixedPoseInBodyFrame());
    // When the slider's translational dof is zero, then P coincides with C.
    // Therefore:
    const RigidTransform<double> X_GF = X_GP * X_FC.inverse();
    return X_GF;
  };
  // Pose of left finger L in gripper frame G when the slider's dof is zero.
  const RigidTransform<double> X_GL(CalcFingerPoseInGripperFrame(left_slider));
  // Pose of right finger R in gripper frame G when the slider's dof is zero.
  const RigidTransform<double> X_GR(CalcFingerPoseInGripperFrame(right_slider));
  // Helper to compute the spatial inertia of a finger F in about the gripper's
  // origin Go, expressed in G.
  auto CalcFingerSpatialInertiaInGripperFrame =
      [](const SpatialInertia<double>& M_FFo_F,
         const RigidTransform<double>& X_GF) {
        const auto M_FFo_G = M_FFo_F.ReExpress(X_GF.rotation());
        const auto p_FoGo_G = -X_GF.translation();
        const auto M_FGo_G = M_FFo_G.Shift(p_FoGo_G);
        return M_FGo_G;
      };
  // Shift and re-express in G frame the finger's spatial inertias.
  const auto M_LGo_G = CalcFingerSpatialInertiaInGripperFrame(M_LLo_L, X_GL);
  const auto M_RGo_G = CalcFingerSpatialInertiaInGripperFrame(M_RRo_R, X_GR);
  // With everything about the same point Go and expressed in the same frame G,
  // proceed to compose into composite body C:
  // TODO(amcastro-tri): Implement operator+() in SpatialInertia.
  SpatialInertia<double> M_CGo_G = M_GGo_G;
  M_CGo_G += M_LGo_G;
  M_CGo_G += M_RGo_G;
  return M_CGo_G;
}

// TODO(russt): Get these from SDF instead of having them hard-coded (#10022).
void get_camera_poses(std::map<std::string, RigidTransform<double>>* pose_map) {
  pose_map->emplace("0", RigidTransform<double>(
                             RollPitchYaw<double>(1.69101, 0.176488, 0.432721),
                             Vector3d(-0.233066, -0.451461, 0.466761)));

  pose_map->emplace("1", RigidTransform<double>(
                             RollPitchYaw<double>(-1.68974, 0.20245, -0.706783),
                             Vector3d(-0.197236, 0.468471, 0.436499)));

  pose_map->emplace("2", RigidTransform<double>(
                             RollPitchYaw<double>(0.0438918, 1.03776, -3.13612),
                             Vector3d(0.786905, -0.0284378, 1.04287)));
}

// Load a SDF model and weld it to the MultibodyPlant.
// @param model_path Full path to the sdf model file. i.e. with
// FindResourceOrThrow
// @param model_name Name of the added model instance.
// @param parent Frame P from the MultibodyPlant to which the new model is
// welded to.
// @param child_frame_name Defines frame C (the child frame), assumed to be
// present in the model being added.
// @param X_PC Transformation of frame C relative to frame P.
template <typename T>
multibody::ModelInstanceIndex AddAndWeldModelFrom(
    const std::string& model_path, const std::string& model_name,
    const multibody::Frame<T>& parent, const std::string& child_frame_name,
    const Isometry3<double>& X_PC, MultibodyPlant<T>* plant) {
  DRAKE_THROW_UNLESS(!plant->HasModelInstanceNamed(model_name));

  multibody::Parser parser(plant);
  const multibody::ModelInstanceIndex new_model =
      parser.AddModelFromFile(model_path, model_name);
  const auto& child_frame = plant->GetFrameByName(child_frame_name, new_model);
  plant->WeldFrames(parent, child_frame, X_PC);
  return new_model;
}

}  // namespace internal

template <typename T>
ManipulationStation<T>::ManipulationStation(double time_step)
    : owned_plant_(std::make_unique<MultibodyPlant<T>>(time_step)),
      owned_scene_graph_(std::make_unique<SceneGraph<T>>()),
      owned_controller_plant_(std::make_unique<MultibodyPlant<T>>()) {
  // Set default gains.
  iiwa_kp_ = VectorXd::Constant(kNumDofIiwa, 100);
  iiwa_ki_ = VectorXd::Constant(kNumDofIiwa, 1);
  iiwa_kd_.resize(kNumDofIiwa);
  for (int i = 0; i < kNumDofIiwa; i++) {
    // Critical damping gains.
    iiwa_kd_[i] = 2 * std::sqrt(iiwa_kp_[i]);
  }

  // This class holds the unique_ptrs explicitly for plant and scene_graph
  // until Finalize() is called (when they are moved into the Diagram). Grab
  // the raw pointers, which should stay valid for the lifetime of the Diagram.
  plant_ = owned_plant_.get();
  scene_graph_ = owned_scene_graph_.get();
  plant_->RegisterAsSourceForSceneGraph(scene_graph_);
  scene_graph_->set_name("scene_graph");

  plant_->template AddForceElement<multibody::UniformGravityFieldElement>(
      -9.81 * Vector3d::UnitZ());
  plant_->set_name("plant");

  this->set_name("manipulation_station");
}

template <typename T>
void ManipulationStation<T>::SetupDefaultStation(
    const IiwaCollisionModel collision_model) {
  // Add the table and 80/20 workcell frame.
  {
    const double dx_table_center_to_robot_base = 0.3257;
    const double dz_table_top_robot_base = 0.0127;
    const std::string sdf_path = FindResourceOrThrow(
        "drake/examples/manipulation_station/models/"
        "amazon_table_simplified.sdf");

    const Isometry3<double> X_WT =
        RigidTransform<double>(Vector3d(dx_table_center_to_robot_base, 0,
                                        -dz_table_top_robot_base))
            .GetAsIsometry3();
    internal::AddAndWeldModelFrom(sdf_path, "table", plant_->world_frame(),
                                  "amazon_table", X_WT, plant_);
  }

  // Add the cupboard.
  {
    const double dx_table_center_to_robot_base = 0.3257;
    const double dz_table_top_robot_base = 0.0127;
    const double dx_cupboard_to_table_center = 0.43 + 0.15;
    const double dz_cupboard_to_table_center = 0.02;
    const double cupboard_height = 0.815;

    const std::string sdf_path = FindResourceOrThrow(
        "drake/examples/manipulation_station/models/cupboard.sdf");

    const Isometry3<double> X_WC =
        RigidTransform<double>(
            RotationMatrix<double>::MakeZRotation(M_PI),
            Vector3d(
                dx_table_center_to_robot_base + dx_cupboard_to_table_center, 0,
                dz_cupboard_to_table_center + cupboard_height / 2.0 -
                    dz_table_top_robot_base))
            .GetAsIsometry3();
    internal::AddAndWeldModelFrom(sdf_path, "cupboard", plant_->world_frame(),
                                  "cupboard_body", X_WC, plant_);
  }

  // Add default iiwa.
  {
    std::string sdf_path;
    switch (collision_model) {
      case IiwaCollisionModel::kNoCollision:
        sdf_path = FindResourceOrThrow(
            "drake/manipulation/models/iiwa_description/iiwa7/"
            "iiwa7_no_collision.sdf");
        break;
      case IiwaCollisionModel::kBoxCollision:
        sdf_path = FindResourceOrThrow(
            "drake/manipulation/models/iiwa_description/iiwa7/"
            "iiwa7_with_box_collision.sdf");
        break;
      default:
        DRAKE_ABORT_MSG("Unrecognized collision_model.");
    }
    const auto X_WI = RigidTransform<double>::Identity();
    auto iiwa_instance = internal::AddAndWeldModelFrom(
        sdf_path, "iiwa", plant_->world_frame(), "iiwa_link_0",
        X_WI.GetAsIsometry3(), plant_);
    RegisterIiwaControllerModel(
        sdf_path, iiwa_instance, plant_->world_frame(),
        plant_->GetFrameByName("iiwa_link_0", iiwa_instance), X_WI);
  }

  // Add default wsg.
  {
    const std::string sdf_path = FindResourceOrThrow(
        "drake/manipulation/models/wsg_50_description/sdf/schunk_wsg_50.sdf");
    const multibody::Frame<T>& link7 =
        plant_->GetFrameByName("iiwa_link_7", iiwa_model_.model_instance);
    const RigidTransform<double> X_7G(RollPitchYaw<double>(M_PI_2, 0, M_PI_2),
                                      Vector3d(0, 0, 0.114));
    auto wsg_instance = internal::AddAndWeldModelFrom(
        sdf_path, "gripper", link7, "body", X_7G.GetAsIsometry3(), plant_);
    RegisterWsgControllerModel(sdf_path, wsg_instance, link7,
                               plant_->GetFrameByName("body", wsg_instance),
                               X_7G);
  }

  // Add default cameras.
  {
    std::map<std::string, RigidTransform<double>> camera_poses;
    internal::get_camera_poses(&camera_poses);
    // Typical D415 intrinsics for 848 x 480 resolution, note that rgb and
    // depth are slightly different. And we are not able to model that at the
    // moment.
    // RGB:
    // - w: 848, h: 480, fx: 616.285, fy: 615.778, ppx: 405.418, ppy: 232.864
    // DEPTH:
    // - w: 848, h: 480, fx: 645.138, fy: 645.138, ppx: 420.789, ppy: 239.13
    // For this camera, we are going to assume that fx = fy, and we can compute
    // fov_y by: fy = height / 2 / tan(fov_y / 2)
    const double kFocalY = 645.;
    const int kHeight = 480;
    const int kWidth = 848;
    const double fov_y = std::atan(kHeight / 2. / kFocalY) * 2;
    geometry::dev::render::DepthCameraProperties camera_properties(
        kWidth, kHeight, fov_y, geometry::dev::render::Fidelity::kLow, 0.1,
        2.0);
    for (const auto& camera_pair : camera_poses) {
      RegisterRgbdCamera(camera_pair.first, plant_->world_frame(),
                         camera_pair.second, camera_properties);
    }
  }
}

template <typename T>
void ManipulationStation<T>::MakeIiwaControllerModel() {
  // Build the controller's version of the plant, which only contains the
  // IIWA and the equivalent inertia of the gripper.
  multibody::Parser parser(owned_controller_plant_.get());
  const auto controller_iiwa_model =
      parser.AddModelFromFile(iiwa_model_.model_path, "iiwa");

  owned_controller_plant_->WeldFrames(
      owned_controller_plant_->world_frame(),
      owned_controller_plant_->GetFrameByName(iiwa_model_.child_frame->name(),
                                              controller_iiwa_model),
      iiwa_model_.X_PC.GetAsIsometry3());
  // Add a single body to represent the IIWA pendant's calibration of the
  // gripper.  The body of the WSG accounts for >90% of the total mass
  // (according to the sdf)... and we don't believe our inertia calibration
  // on the hardware to be so precise, so we simply ignore the inertia
  // contribution from the fingers here.
  const multibody::RigidBody<T>& wsg_equivalent =
      owned_controller_plant_->AddRigidBody(
          "wsg_equivalent", controller_iiwa_model,
          internal::MakeCompositeGripperInertia(
              wsg_model_.model_path, wsg_model_.child_frame->name()));

  // TODO(siyuan.feng@tri.global): when we handle multiple IIWA and WSG, this
  // part need to deal with the parent's (iiwa's) model instance id.
  owned_controller_plant_->WeldFrames(
      owned_controller_plant_->GetFrameByName(wsg_model_.parent_frame->name(),
                                              controller_iiwa_model),
      wsg_equivalent.body_frame(), wsg_model_.X_PC.GetAsIsometry3());

  owned_controller_plant_
      ->template AddForceElement<multibody::UniformGravityFieldElement>(
          -9.81 * Vector3d::UnitZ());
  owned_controller_plant_->set_name("controller_plant");
}

template <typename T>
void ManipulationStation<T>::Finalize() {
  DRAKE_THROW_UNLESS(iiwa_model_.model_instance.is_valid());
  DRAKE_THROW_UNLESS(wsg_model_.model_instance.is_valid());

  MakeIiwaControllerModel();

  // Note: This deferred diagram construction method/workflow exists because we
  //   - cannot finalize plant until all of my objects are added, and
  //   - cannot wire up my diagram until we have finalized the plant.
  plant_->Finalize();

  systems::DiagramBuilder<T> builder;

  builder.AddSystem(std::move(owned_plant_));
  builder.AddSystem(std::move(owned_scene_graph_));

  builder.Connect(
      plant_->get_geometry_poses_output_port(),
      scene_graph_->get_source_pose_port(plant_->get_source_id().value()));
  builder.Connect(scene_graph_->get_query_output_port(),
                  plant_->get_geometry_query_input_port());

  // Export the commanded positions via a PassThrough.
  auto iiwa_position =
      builder.template AddSystem<systems::PassThrough>(kNumDofIiwa);
  builder.ExportInput(iiwa_position->get_input_port(), "iiwa_position");
  builder.ExportOutput(iiwa_position->get_output_port(),
                       "iiwa_position_commanded");

  // Export iiwa "state" outputs.
  {
    auto demux = builder.template AddSystem<systems::Demultiplexer>(
        2 * kNumDofIiwa, kNumDofIiwa);
    builder.Connect(
        plant_->get_continuous_state_output_port(iiwa_model_.model_instance),
        demux->get_input_port(0));
    builder.ExportOutput(demux->get_output_port(0), "iiwa_position_measured");
    builder.ExportOutput(demux->get_output_port(1), "iiwa_velocity_estimated");

    builder.ExportOutput(
        plant_->get_continuous_state_output_port(iiwa_model_.model_instance),
        "iiwa_state_estimated");
  }

  // Add the IIWA controller "stack".
  {
    owned_controller_plant_->Finalize();

    // Add the inverse dynamics controller.
    auto iiwa_controller = builder.template AddSystem<
        systems::controllers::InverseDynamicsController>(
        *owned_controller_plant_, iiwa_kp_, iiwa_ki_, iiwa_kd_, false);
    iiwa_controller->set_name("iiwa_controller");
    builder.Connect(
        plant_->get_continuous_state_output_port(iiwa_model_.model_instance),
        iiwa_controller->get_input_port_estimated_state());

    // Add in feedforward torque.
    auto adder = builder.template AddSystem<systems::Adder>(2, kNumDofIiwa);
    builder.Connect(iiwa_controller->get_output_port_control(),
                    adder->get_input_port(0));
    builder.ExportInput(adder->get_input_port(1), "iiwa_feedforward_torque");
    builder.Connect(
        adder->get_output_port(),
        plant_->get_actuation_input_port(iiwa_model_.model_instance));

    // Approximate desired state command from a discrete derivative of the
    // position command input port.
    auto desired_state_from_position = builder.template AddSystem<
        systems::StateInterpolatorWithDiscreteDerivative>(kNumDofIiwa,
                                                          plant_->time_step());
    desired_state_from_position->set_name("desired_state_from_position");
    builder.Connect(desired_state_from_position->get_output_port(),
                    iiwa_controller->get_input_port_desired_state());
    builder.Connect(iiwa_position->get_output_port(),
                    desired_state_from_position->get_input_port());

    // Export commanded torques:
    builder.ExportOutput(adder->get_output_port(), "iiwa_torque_commanded");
    builder.ExportOutput(adder->get_output_port(), "iiwa_torque_measured");
  }

  {
    auto wsg_controller = builder.template AddSystem<
        manipulation::schunk_wsg::SchunkWsgPositionController>(
        manipulation::schunk_wsg::kSchunkWsgLcmStatusPeriod, wsg_kp_, wsg_kd_);
    wsg_controller->set_name("wsg_controller");

    builder.Connect(
        wsg_controller->get_generalized_force_output_port(),
        plant_->get_actuation_input_port(wsg_model_.model_instance));
    builder.Connect(
        plant_->get_continuous_state_output_port(wsg_model_.model_instance),
        wsg_controller->get_state_input_port());

    builder.ExportInput(wsg_controller->get_desired_position_input_port(),
                        "wsg_position");
    builder.ExportInput(wsg_controller->get_force_limit_input_port(),
                        "wsg_force_limit");

    auto wsg_mbp_state_to_wsg_state = builder.template AddSystem(
        manipulation::schunk_wsg::MakeMultibodyStateToWsgStateSystem<double>());
    builder.Connect(
        plant_->get_continuous_state_output_port(wsg_model_.model_instance),
        wsg_mbp_state_to_wsg_state->get_input_port());

    builder.ExportOutput(wsg_mbp_state_to_wsg_state->get_output_port(),
                         "wsg_state_measured");

    builder.ExportOutput(wsg_controller->get_grip_force_output_port(),
                         "wsg_force_measured");
  }

  builder.ExportOutput(plant_->get_generalized_contact_forces_output_port(
                           iiwa_model_.model_instance),
                       "iiwa_torque_external");

  {  // RGB-D Cameras
    render_scene_graph_ =
        builder.template AddSystem<geometry::dev::SceneGraph>(*scene_graph_);
    render_scene_graph_->set_name("dev_scene_graph_for_rendering");

    builder.Connect(plant_->get_geometry_poses_output_port(),
                    render_scene_graph_->get_source_pose_port(
                        plant_->get_source_id().value()));

    for (const auto& info_pair : camera_information_) {
      std::string camera_name = "camera_" + info_pair.first;
      const CameraInformation& info = info_pair.second;

      const optional<geometry::FrameId> parent_body_id =
          plant_->GetBodyFrameIdIfExists(info.parent_frame->body().index());
      DRAKE_THROW_UNLESS(parent_body_id.has_value());
      const Isometry3<double> X_PC =
          info.parent_frame->GetFixedPoseInBodyFrame() *
          info.X_PC.GetAsIsometry3();

      auto camera =
          builder.template AddSystem<systems::sensors::dev::RgbdCamera>(
              camera_name, parent_body_id.value(), X_PC, info.properties,
              false);
      builder.Connect(render_scene_graph_->get_query_output_port(),
                      camera->query_object_input_port());

      builder.ExportOutput(camera->color_image_output_port(),
                           camera_name + "_rgb_image");
      builder.ExportOutput(camera->GetOutputPort("depth_image_16u"),
                           camera_name + "_depth_image");
      builder.ExportOutput(camera->label_image_output_port(),
                           camera_name + "_label_image");
    }
  }

  builder.ExportOutput(scene_graph_->get_pose_bundle_output_port(),
                       "pose_bundle");

  builder.ExportOutput(plant_->get_contact_results_output_port(),
                       "contact_results");
  builder.ExportOutput(plant_->get_continuous_state_output_port(),
                       "plant_continuous_state");
  builder.ExportOutput(plant_->get_geometry_poses_output_port(),
                       "geometry_poses");

  builder.BuildInto(this);
}

template <typename T>
VectorX<T> ManipulationStation<T>::GetIiwaPosition(
    const systems::Context<T>& station_context) const {
  const auto& plant_context =
      this->GetSubsystemContext(*plant_, station_context);
  // TODO(russt): update upon resolution of #9623.
  VectorX<T> q(kNumDofIiwa);
  for (int i = 0; i < kNumDofIiwa; i++) {
    q(i) = plant_
               ->template GetJointByName<RevoluteJoint>("iiwa_joint_" +
                                                        std::to_string(i + 1))
               .get_angle(plant_context);
  }
  return q;
}

template <typename T>
void ManipulationStation<T>::SetIiwaPosition(
    const Eigen::Ref<const drake::VectorX<T>>& q,
    drake::systems::Context<T>* station_context) const {
  DRAKE_DEMAND(station_context != nullptr);
  DRAKE_DEMAND(q.size() == kNumDofIiwa);
  auto& plant_context =
      this->GetMutableSubsystemContext(*plant_, station_context);
  // TODO(russt): update upon resolution of #9623.
  for (int i = 0; i < kNumDofIiwa; i++) {
    plant_
        ->template GetJointByName<RevoluteJoint>("iiwa_joint_" +
                                                 std::to_string(i + 1))
        .set_angle(&plant_context, q(i));
  }

  // Set the position history in the state interpolator to match.
  const auto& state_from_position =
      dynamic_cast<
          const systems::StateInterpolatorWithDiscreteDerivative<double>&>(this
          ->GetSubsystemByName("desired_state_from_position"));
  state_from_position.set_initial_position(
      &this->GetMutableSubsystemContext(state_from_position, station_context),
      q);
}

template <typename T>
VectorX<T> ManipulationStation<T>::GetIiwaVelocity(
    const systems::Context<T>& station_context) const {
  const auto& plant_context =
      this->GetSubsystemContext(*plant_, station_context);
  VectorX<T> v(kNumDofIiwa);
  // TODO(russt): update upon resolution of #9623.
  for (int i = 0; i < kNumDofIiwa; i++) {
    v(i) = plant_
               ->template GetJointByName<RevoluteJoint>("iiwa_joint_" +
                                                        std::to_string(i + 1))
               .get_angular_rate(plant_context);
  }
  return v;
}

template <typename T>
void ManipulationStation<T>::SetIiwaVelocity(
    const Eigen::Ref<const drake::VectorX<T>>& v,
    drake::systems::Context<T>* station_context) const {
  DRAKE_DEMAND(station_context != nullptr);
  DRAKE_DEMAND(v.size() == kNumDofIiwa);
  auto& plant_context =
      this->GetMutableSubsystemContext(*plant_, station_context);
  // TODO(russt): update upon resolution of #9623.
  for (int i = 0; i < kNumDofIiwa; i++) {
    plant_
        ->template GetJointByName<RevoluteJoint>("iiwa_joint_" +
                                                 std::to_string(i + 1))
        .set_angular_rate(&plant_context, v(i));
  }
}

template <typename T>
T ManipulationStation<T>::GetWsgPosition(
    const systems::Context<T>& station_context) const {
  const auto& plant_context =
      this->GetSubsystemContext(*plant_, station_context);

  // TODO(russt): update upon resolution of #9623.
  return plant_
             ->template GetJointByName<PrismaticJoint>(
                 "right_finger_sliding_joint", wsg_model_.model_instance)
             .get_translation(plant_context) -
         plant_
             ->template GetJointByName<PrismaticJoint>(
                 "left_finger_sliding_joint", wsg_model_.model_instance)
             .get_translation(plant_context);
}

template <typename T>
T ManipulationStation<T>::GetWsgVelocity(
    const systems::Context<T>& station_context) const {
  const auto& plant_context =
      this->GetSubsystemContext(*plant_, station_context);

  // TODO(russt): update upon resolution of #9623.
  return plant_
             ->template GetJointByName<PrismaticJoint>(
                 "right_finger_sliding_joint", wsg_model_.model_instance)
             .get_translation_rate(plant_context) -
         plant_
             ->template GetJointByName<PrismaticJoint>(
                 "left_finger_sliding_joint", wsg_model_.model_instance)
             .get_translation_rate(plant_context);
}

template <typename T>
void ManipulationStation<T>::SetWsgPosition(
    const T& q, drake::systems::Context<T>* station_context) const {
  auto& plant_context =
      this->GetMutableSubsystemContext(*plant_, station_context);

  // TODO(russt): update upon resolution of #9623.
  plant_
      ->template GetJointByName<PrismaticJoint>("right_finger_sliding_joint",
                                                wsg_model_.model_instance)
      .set_translation(&plant_context, q / 2);
  plant_
      ->template GetJointByName<PrismaticJoint>("left_finger_sliding_joint",
                                                wsg_model_.model_instance)
      .set_translation(&plant_context, -q / 2);

  // Set the position history in the state interpolator to match.
  const auto& wsg_controller = dynamic_cast<
      const manipulation::schunk_wsg::SchunkWsgPositionController&>(
      this->GetSubsystemByName("wsg_controller"));
  wsg_controller.set_initial_position(
      &this->GetMutableSubsystemContext(wsg_controller, station_context), q);
}

template <typename T>
void ManipulationStation<T>::SetWsgVelocity(
    const T& v, drake::systems::Context<T>* station_context) const {
  auto& plant_context =
      this->GetMutableSubsystemContext(*plant_, station_context);

  // TODO(russt): update upon resolution of #9623.
  plant_
      ->template GetJointByName<PrismaticJoint>("right_finger_sliding_joint",
                                                wsg_model_.model_instance)
      .set_translation_rate(&plant_context, v / 2);
  plant_
      ->template GetJointByName<PrismaticJoint>("left_finger_sliding_joint",
                                                wsg_model_.model_instance)
      .set_translation_rate(&plant_context, -v / 2);
}

template <typename T>
std::vector<std::string> ManipulationStation<T>::get_camera_names() const {
  std::vector<std::string> names;
  names.reserve(camera_information_.size());
  for (const auto& info : camera_information_) {
    names.emplace_back(info.first);
  }
  return names;
}

template <typename T>
void ManipulationStation<T>::SetWsgGains(const double kp, const double kd) {
  DRAKE_THROW_UNLESS(!plant_->is_finalized());
  DRAKE_THROW_UNLESS(kp >= 0 && kd >= 0);
  wsg_kp_ = kp;
  wsg_kd_ = kd;
}

template <typename T>
void ManipulationStation<T>::SetIiwaGains(const VectorX<double>& new_gains,
                                          VectorX<double>* gains) const {
  DRAKE_THROW_UNLESS(!plant_->is_finalized());
  DRAKE_THROW_UNLESS(new_gains.size() == gains->size());
  DRAKE_THROW_UNLESS((new_gains.array() >= 0).all());
  *gains = new_gains;
}

template <typename T>
void ManipulationStation<T>::RegisterIiwaControllerModel(
    const std::string& model_path,
    const multibody::ModelInstanceIndex iiwa_instance,
    const multibody::Frame<T>& parent_frame,
    const multibody::Frame<T>& child_frame,
    const RigidTransform<double>& X_PC) {
  // TODO(siyuan.feng@tri.global): We really only just need to make sure
  // the parent frame is a AnchoredFrame(i.e. there is a rigid kinematic path
  // from it to the world), and record that X_WP. However, the computation to
  // query X_WP given a partially constructed plant is not feasible at the
  // moment, so we are forcing the parent frame to be the world instead.
  DRAKE_THROW_UNLESS(parent_frame.name() == plant_->world_frame().name());

  iiwa_model_.model_path = model_path;
  iiwa_model_.parent_frame = &parent_frame;
  iiwa_model_.child_frame = &child_frame;
  iiwa_model_.X_PC = X_PC;

  iiwa_model_.model_instance = iiwa_instance;
}

template <typename T>
void ManipulationStation<T>::RegisterWsgControllerModel(
    const std::string& model_path,
    const multibody::ModelInstanceIndex wsg_instance,
    const multibody::Frame<T>& parent_frame,
    const multibody::Frame<T>& child_frame,
    const RigidTransform<double>& X_PC) {
  wsg_model_.model_path = model_path;
  wsg_model_.parent_frame = &parent_frame;
  wsg_model_.child_frame = &child_frame;
  wsg_model_.X_PC = X_PC;

  wsg_model_.model_instance = wsg_instance;
}

template <typename T>
void ManipulationStation<T>::RegisterRgbdCamera(
    const std::string& name, const multibody::Frame<T>& parent_frame,
    const RigidTransform<double>& X_PC,
    const geometry::dev::render::DepthCameraProperties& properties) {
  CameraInformation info;
  info.parent_frame = &parent_frame;
  info.X_PC = X_PC;
  info.properties = properties;

  camera_information_[name] = info;
}

template <typename T>
std::map<std::string, RigidTransform<double>>
ManipulationStation<T>::GetStaticCameraPosesInWorld() const {
  std::map<std::string, RigidTransform<double>> static_camera_poses;

  for (const auto& info : camera_information_) {
    const auto& frame_P = *info.second.parent_frame;

    // TODO(siyuan.feng@tri.global): We really only just need to make sure
    // the parent frame is a AnchoredFrame(i.e. there is a rigid kinematic path
    // from it to the world). However, the computation to query X_WP given a
    // partially constructed plant is not feasible at the moment, so we are
    // looking for cameras that are directly attached to the world instead.
    const bool is_anchored =
        frame_P.body().index() == plant_->world_frame().body().index();
    if (is_anchored) {
      static_camera_poses.emplace(
          info.first,
          RigidTransform<double>(frame_P.GetFixedPoseInBodyFrame()) *
              info.second.X_PC);
    }
  }

  return static_camera_poses;
}

}  // namespace manipulation_station
}  // namespace examples
}  // namespace drake

// TODO(russt): Support at least NONSYMBOLIC_SCALARS.  See #9573.
//   (and don't forget to include default_scalars.h)
template class ::drake::examples::manipulation_station::ManipulationStation<
    double>;
