//-- includes -----
#include "DeviceManager.h"
#include "ServerHMDView.h"
#include "MathAlignment.h"
#include "MorpheusHMD.h"
#include "OrientationFilter.h"
#include "PositionFilter.h"
#include "PSMoveProtocol.pb.h"
#include "ServerRequestHandler.h"
#include "ServerTrackerView.h"
#include "TrackerManager.h"

//-- constants -----
static const float k_min_time_delta_seconds = 1 / 120.f;
static const float k_max_time_delta_seconds = 1 / 30.f;

//-- private methods -----
static void init_filters_for_morpheus_hmd(
    const MorpheusHMD *morpheusHMD,
    OrientationFilter *orientation_filter, PositionFilter *position_filter);
static void update_filters_for_morpheus_hmd(
	const MorpheusHMD *morpheusHMD, const MorpheusHMDState *morpheusHMDState,
	const float delta_time, const HMDOpticalPoseEstimation *poseEstimation,
	OrientationFilter *orientationFilter, PositionFilter *position_filter);
static void generate_morpheus_hmd_data_frame_for_stream(
    const ServerHMDView *hmd_view, const HMDStreamInfo *stream_info,
    DeviceOutputDataFramePtr &data_frame);

static Eigen::Vector3f CommonDevicePosition_to_EigenVector3f(const CommonDevicePosition &p);
static Eigen::Vector3f CommonDeviceVector_to_EigenVector3f(const CommonDeviceVector &v);
static Eigen::Quaternionf CommonDeviceQuaternion_to_EigenQuaternionf(const CommonDeviceQuaternion &q);

static CommonDevicePosition EigenVector3f_to_CommonDevicePosition(const Eigen::Vector3f &p);
static CommonDeviceQuaternion EigenQuaternionf_to_CommonDeviceQuaternion(const Eigen::Quaternionf &q);

//-- public implementation -----
ServerHMDView::ServerHMDView(const int device_id)
	: ServerDeviceView(device_id)
	, m_tracking_listener_count(0)
	, m_tracking_enabled(false)
	, m_device(nullptr)
	, m_tracker_pose_estimation(nullptr)
	, m_multicam_pose_estimation(nullptr)
	, m_orientation_filter(nullptr)
	, m_position_filter(nullptr)
	, m_lastPollSeqNumProcessed(-1)
	, m_last_filter_update_timestamp()
	, m_last_filter_update_timestamp_valid(false)
{
}

ServerHMDView::~ServerHMDView()
{
    if (m_orientation_filter != nullptr)
    {
        delete m_orientation_filter;
        m_orientation_filter = nullptr;
    }

    if (m_position_filter != nullptr)
    {
        delete m_position_filter;
        m_position_filter = nullptr;
    }

    if (m_device != nullptr)
    {
        delete m_device;
    }
}

bool ServerHMDView::allocate_device_interface(const class DeviceEnumerator *enumerator)
{
    switch (enumerator->get_device_type())
    {
    case CommonDeviceState::Morpheus:
        {
            m_device = new MorpheusHMD();
            m_orientation_filter = new OrientationFilter();
            m_position_filter = new PositionFilter();
        } break;
    default:
        break;
    }

    return m_device != nullptr;
}

void ServerHMDView::free_device_interface()
{
    if (m_device != nullptr)
    {
        delete m_device;
        m_device = nullptr;
    }
}

bool ServerHMDView::open(const class DeviceEnumerator *enumerator)
{
    // Attempt to open the controller
    bool bSuccess = ServerDeviceView::open(enumerator);

    // Setup the orientation filter based on the controller configuration
    if (bSuccess)
    {
        IDeviceInterface *device = getDevice();

        switch (device->getDeviceType())
        {
        case CommonDeviceState::Morpheus:
            {
                const MorpheusHMD *morpheusHMD = this->castCheckedConst<MorpheusHMD>();

                init_filters_for_morpheus_hmd(morpheusHMD, m_orientation_filter, m_position_filter);
            } break;
        default:
            break;
        }

        // Reset the poll sequence number high water mark
        m_lastPollSeqNumProcessed = -1;
    }

    return bSuccess;
}

void ServerHMDView::updateOpticalPoseEstimation(TrackerManager* tracker_manager)
{
	const std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();

	// TODO: Probably need to first update IMU state to get velocity.
	// If velocity is too high, don't bother getting a new position.
	// Though it may be enough to just use the camera ROI as the limit.

	if (getIsTrackingEnabled())
	{
		Eigen::Quaternionf hmd_world_orientations[TrackerManager::k_max_devices];
		float hmd_orientation_weights[TrackerManager::k_max_devices];
		int orientations_found = 0;

		int valid_position_tracker_ids[TrackerManager::k_max_devices];
		int positions_found = 0;

		float screen_area_sum = 0;

		// Compute an estimated 3d tracked position of the controller 
		// from the perspective of each tracker
		for (int tracker_id = 0; tracker_id < tracker_manager->getMaxDevices(); ++tracker_id)
		{
			ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);
			HMDOpticalPoseEstimation &trackerPoseEstimateRef = m_tracker_pose_estimation[tracker_id];

			const bool bWasTracking = trackerPoseEstimateRef.bCurrentlyTracking;

			// Assume we're going to lose tracking this frame
			trackerPoseEstimateRef.bCurrentlyTracking = false;

			if (tracker->getIsOpen())
			{
				// See how long it's been since we got a new video frame
				const std::chrono::time_point<std::chrono::high_resolution_clock> now =
					std::chrono::high_resolution_clock::now();
				const std::chrono::duration<float, std::milli> timeSinceNewDataMillis =
					now - tracker->getLastNewDataTimestamp();
				const float timeoutMilli =
					static_cast<float>(DeviceManager::getInstance()->m_tracker_manager->getConfig().optical_tracking_timeout);

				// Can't compute tracking on video data that's too old
				if (timeSinceNewDataMillis.count() < timeoutMilli)
				{
					// Initially the newTrackerPoseEstimate is a copy of the existing pose
					bool bIsVisibleThisUpdate = false;

					// If a new video frame is available this tick, 
					// attempt to update the tracking location
					if (tracker->getHasUnpublishedState())
					{
						HMDOpticalPoseEstimation newTrackerPoseEstimate = trackerPoseEstimateRef;
						CommonDevicePose poseGuess = { trackerPoseEstimateRef.position, trackerPoseEstimateRef.orientation };

						if (tracker->computePoseForHMD(
							this,
							trackerPoseEstimateRef.bOrientationValid ? &poseGuess : nullptr,
							&newTrackerPoseEstimate))
						{
							bIsVisibleThisUpdate = true;

							trackerPoseEstimateRef = newTrackerPoseEstimate;
							trackerPoseEstimateRef.last_visible_timestamp = now;
						}
					}

					// If the position estimate isn't too old (or updated this tick), 
					// say we have a valid tracked location
					if (bWasTracking || bIsVisibleThisUpdate)
					{
						const std::chrono::duration<float, std::milli> timeSinceLastVisibleMillis =
							now - trackerPoseEstimateRef.last_visible_timestamp;

						if (timeSinceLastVisibleMillis.count() < timeoutMilli)
						{
							const float tracker_screen_area = trackerPoseEstimateRef.projection.screen_area;

							// Sum up the tracking screen area over all of the trackers that can see the controller
							screen_area_sum += tracker_screen_area;

							// If this tracker has a valid position for the controller
							// add it to the tracker id list
							valid_position_tracker_ids[positions_found] = tracker_id;
							++positions_found;

							// If the pose has a valid tracker relative orientation,
							// convert the orientation to world space and add it
							// to a weighted list of orientations
							if (trackerPoseEstimateRef.bOrientationValid)
							{
								const CommonDeviceQuaternion &tracker_relative_quaternion = trackerPoseEstimateRef.orientation;
								const CommonDeviceQuaternion &world_quaternion =
									tracker->computeWorldOrientation(&tracker_relative_quaternion);
								const Eigen::Quaternionf eigen_quaternion(
									world_quaternion.w, world_quaternion.x, world_quaternion.y, world_quaternion.z);

								hmd_world_orientations[orientations_found] = eigen_quaternion;
								hmd_orientation_weights[orientations_found] = tracker_screen_area;
								++orientations_found;
							}

							// Flag this pose estimate as invalid
							trackerPoseEstimateRef.bCurrentlyTracking = true;
						}
					}
				}
			}

			// Keep track of the last time the position estimate was updated
			trackerPoseEstimateRef.last_update_timestamp = now;
			trackerPoseEstimateRef.bValidTimestamps = true;
		}

		// If multiple trackers can see the controller, 
		// triangulate all pairs of trackers and average the results
		if (positions_found > 1)
		{
			// Project the tracker relative 3d tracking position back on to the tracker camera plane
			CommonDeviceScreenLocation position2d_list[TrackerManager::k_max_devices];
			for (int list_index = 0; list_index < positions_found; ++list_index)
			{
				const int tracker_id = valid_position_tracker_ids[list_index];
				const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);
				const HMDOpticalPoseEstimation &positionEstimate = m_tracker_pose_estimation[tracker_id];

				position2d_list[list_index] = tracker->projectTrackerRelativePosition(&positionEstimate.position);
			}

			int pair_count = 0;
			CommonDevicePosition average_world_position = { 0.f, 0.f, 0.f };
			for (int list_index = 0; list_index < positions_found; ++list_index)
			{
				const int tracker_id = valid_position_tracker_ids[list_index];
				const CommonDeviceScreenLocation &screen_location = position2d_list[list_index];
				const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);

				for (int other_list_index = list_index + 1; other_list_index < positions_found; ++other_list_index)
				{
					const int other_tracker_id = valid_position_tracker_ids[other_list_index];
					const CommonDeviceScreenLocation &other_screen_location = position2d_list[other_list_index];
					const ServerTrackerViewPtr other_tracker = tracker_manager->getTrackerViewPtr(other_tracker_id);

					// Using the screen locations on two different trackers we can triangulate a world position
					CommonDevicePosition world_position =
						ServerTrackerView::triangulateWorldPosition(
							tracker.get(), &screen_location,
							other_tracker.get(), &other_screen_location);

					average_world_position.x += world_position.x;
					average_world_position.y += world_position.y;
					average_world_position.z += world_position.z;

					++pair_count;
				}
			}

			if (pair_count > 1)
			{
				const float N = static_cast<float>(pair_count);

				average_world_position.x /= N;
				average_world_position.y /= N;
				average_world_position.z /= N;
			}

			// Store the averaged tracking position
			m_multicam_pose_estimation->position = average_world_position;
			m_multicam_pose_estimation->bCurrentlyTracking = true;

			// Compute the average projection area.
			// This is proportional to our position tracking quality.
			m_multicam_pose_estimation->projection.screen_area =
				screen_area_sum / static_cast<float>(positions_found);
		}
		// If only one tracker can see the controller, then just use the position estimate from that
		else if (positions_found == 1)
		{
			// Put the tracker relative position into world space
			const int tracker_id = valid_position_tracker_ids[0];
			const ServerTrackerViewPtr tracker = tracker_manager->getTrackerViewPtr(tracker_id);
			const CommonDevicePosition &tracker_relative_position = m_tracker_pose_estimation[tracker_id].position;

			// Only one tracker can see the controller
			m_multicam_pose_estimation->position = tracker->computeWorldPosition(&tracker_relative_position);
			m_multicam_pose_estimation->bCurrentlyTracking = true;

			// The average screen area is just the sum
			m_multicam_pose_estimation->projection.screen_area = screen_area_sum;
		}
		// If no trackers can see the controller, maintain the last known position and time it was seen
		else
		{
			m_multicam_pose_estimation->bCurrentlyTracking = false;
		}

		// Compute a weighted average of all of the orientations we found.
		// Weighted by the projection area (higher projection area proportional to better quality orientation)        
		if (orientations_found > 0)
		{
			Eigen::Quaternionf avg_world_orientation;

			if (eigen_quaternion_compute_weighted_average(
				hmd_world_orientations,
				hmd_orientation_weights,
				orientations_found,
				&avg_world_orientation))
			{
				m_multicam_pose_estimation->orientation.w = avg_world_orientation.w();
				m_multicam_pose_estimation->orientation.x = avg_world_orientation.x();
				m_multicam_pose_estimation->orientation.y = avg_world_orientation.y();
				m_multicam_pose_estimation->orientation.z = avg_world_orientation.z();
				m_multicam_pose_estimation->bOrientationValid = true;
			}
			else
			{
				m_multicam_pose_estimation->bOrientationValid = false;
			}
		}
		else
		{
			m_multicam_pose_estimation->bOrientationValid = false;
		}

		// Update the position estimation timestamps
		if (positions_found > 0)
		{
			m_multicam_pose_estimation->last_visible_timestamp = now;
		}
		m_multicam_pose_estimation->last_update_timestamp = now;
		m_multicam_pose_estimation->bValidTimestamps = true;
	}
}

void ServerHMDView::updateStateAndPredict()
{
	if (!getHasUnpublishedState())
	{
		return;
	}

	// Look backward in time to find the first HMD update state with a poll sequence number 
	// newer than the last sequence number we've processed.
	int firstLookBackIndex = -1;
	int testLookBack = 0;
	const CommonHMDState *state = getState(testLookBack);
	while (state != nullptr && state->PollSequenceNumber > m_lastPollSeqNumProcessed)
	{
		firstLookBackIndex = testLookBack;
		testLookBack++;
		state = getState(testLookBack);
	}
	assert(firstLookBackIndex >= 0);

	// Compute the time in seconds since the last update
	const std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();
	float time_delta_seconds;
	if (m_last_filter_update_timestamp_valid)
	{
		const std::chrono::duration<float, std::milli> time_delta = now - m_last_filter_update_timestamp;
		const float time_delta_milli = time_delta.count();

		// convert delta to seconds clamp time delta between 120hz and 30hz
		time_delta_seconds = clampf(time_delta_milli / 1000.f, k_min_time_delta_seconds, k_max_time_delta_seconds);
	}
	else
	{
		time_delta_seconds = k_max_time_delta_seconds;
	}
	m_last_filter_update_timestamp = now;
	m_last_filter_update_timestamp_valid = true;

	// Evenly apply the list of hmd state updates over the time since last filter update
	float per_state_time_delta_seconds = time_delta_seconds / static_cast<float>(firstLookBackIndex + 1);

	// Process the polled hmd states forward in time
	// computing the new orientation along the way.
	for (int lookBackIndex = firstLookBackIndex; lookBackIndex >= 0; --lookBackIndex)
	{
		const CommonHMDState *hmdState = getState(lookBackIndex);

		switch (hmdState->DeviceType)
		{
		case CommonHMDState::Morpheus:
		{
			const MorpheusHMD *morpheusHMD = this->castCheckedConst<MorpheusHMD>();
			const MorpheusHMDState *morpheusHMDState = static_cast<const MorpheusHMDState *>(hmdState);

			// Only update the position filter when tracking is enabled
			update_filters_for_morpheus_hmd(
				morpheusHMD, morpheusHMDState,
				per_state_time_delta_seconds,
				m_multicam_pose_estimation,
				m_orientation_filter,
				getIsTrackingEnabled() ? m_position_filter : nullptr);
		} break;
		default:
			assert(0 && "Unhandled HMD type");
		}

		// Consider this hmd state sequence num processed
		m_lastPollSeqNumProcessed = hmdState->PollSequenceNumber;
	}
}

CommonDevicePose
ServerHMDView::getFilteredPose(float time) const
{
	CommonDevicePose pose;

	pose.clear();

	if (m_orientation_filter != nullptr)
	{
		Eigen::Quaternionf orientation = m_orientation_filter->getOrientation(time);

		pose.Orientation.w = orientation.w();
		pose.Orientation.x = orientation.x();
		pose.Orientation.y = orientation.y();
		pose.Orientation.z = orientation.z();
	}

	if (m_position_filter != nullptr)
	{
		Eigen::Vector3f position = m_position_filter->getPosition(time);

		pose.Position.x = position.x();
		pose.Position.y = position.y();
		pose.Position.z = position.z();
	}

	return pose;
}

CommonDevicePhysics
ServerHMDView::getFilteredPhysics() const
{
	CommonDevicePhysics physics;

	if (m_orientation_filter != nullptr)
	{
		const Eigen::Vector3f first_derivative = m_orientation_filter->getAngularVelocity();
		const Eigen::Vector3f second_derivative = m_orientation_filter->getAngularAcceleration();

		physics.AngularVelocity.i = first_derivative.x();
		physics.AngularVelocity.j = first_derivative.y();
		physics.AngularVelocity.k = first_derivative.z();

		physics.AngularAcceleration.i = second_derivative.x();
		physics.AngularAcceleration.j = second_derivative.y();
		physics.AngularAcceleration.k = second_derivative.z();
	}

	if (m_position_filter != nullptr)
	{
		Eigen::Vector3f velocity(m_position_filter->getVelocity());
		Eigen::Vector3f acceleration(m_position_filter->getAcceleration());

		physics.Velocity.i = velocity.x();
		physics.Velocity.j = velocity.y();
		physics.Velocity.k = velocity.z();

		physics.Acceleration.i = acceleration.x();
		physics.Acceleration.j = acceleration.y();
		physics.Acceleration.k = acceleration.z();
	}

	return physics;
}

// Returns the full usb device path for the controller
std::string
ServerHMDView::getUSBDevicePath() const
{
    return m_device->getUSBDevicePath();
}

// Returns the "controller_" + serial number for the controller
std::string
ServerHMDView::getConfigIdentifier() const
{
	std::string	identifier = "";

	if (m_device != nullptr)
	{
		if (m_device->getDeviceType() == CommonDeviceState::Morpheus)
		{
			identifier = "hmd_morpheus";
		}
		else
		{
			identifier = "hmd_unknown";
		}
	}

	return identifier;
}

CommonDeviceState::eDeviceType
ServerHMDView::getHMDDeviceType() const
{
    return m_device->getDeviceType();
}

// Fetch the controller state at the given sample index.
// A lookBack of 0 corresponds to the most recent data.
const struct CommonHMDState * ServerHMDView::getState(
    int lookBack) const
{
    const struct CommonDeviceState *device_state = m_device->getState(lookBack);
    assert(device_state == nullptr ||
        (device_state->DeviceType >= CommonDeviceState::HeadMountedDisplay &&
        device_state->DeviceType < CommonDeviceState::SUPPORTED_HMD_TYPE_COUNT));

    return static_cast<const CommonHMDState *>(device_state);
}

void ServerHMDView::startTracking()
{
	if (!m_tracking_enabled)
	{
		set_tracking_enabled_internal(true);
	}

	++m_tracking_listener_count;
}

void ServerHMDView::stopTracking()
{
	assert(m_tracking_listener_count > 0);
	--m_tracking_listener_count;

	if (m_tracking_listener_count <= 0 && m_tracking_enabled)
	{
		set_tracking_enabled_internal(false);
	}
}

void ServerHMDView::set_tracking_enabled_internal(bool bEnabled)
{
	if (m_tracking_enabled != bEnabled)
	{
		if (bEnabled)
		{
			// Start tracking setup
		}
		else
		{
			// Stop tracking teardown
		}

		m_tracking_enabled = bEnabled;
	}
}

// Get the tracking shape for the controller
bool ServerHMDView::getTrackingShape(CommonDeviceTrackingShape &trackingShape) const
{
	m_device->getTrackingShape(trackingShape);

	return trackingShape.shape_type != eCommonTrackingShapeType::INVALID_SHAPE;
}

eCommonTrackingColorID ServerHMDView::getTrackingColorID() const
{
	eCommonTrackingColorID tracking_color_id = eCommonTrackingColorID::INVALID_COLOR;

	if (m_device != nullptr)
	{
		m_device->getTrackingColorID(tracking_color_id);
	}

	return tracking_color_id;
}

void ServerHMDView::publish_device_data_frame()
{
    // Tell the server request handler we want to send out HMD updates.
    // This will call generate_hmd_data_frame_for_stream for each listening connection.
    ServerRequestHandler::get_instance()->publish_hmd_data_frame(
        this, &ServerHMDView::generate_hmd_data_frame_for_stream);
}

void ServerHMDView::generate_hmd_data_frame_for_stream(
    const ServerHMDView *hmd_view,
    const struct HMDStreamInfo *stream_info,
    DeviceOutputDataFramePtr &data_frame)
{
    PSMoveProtocol::DeviceOutputDataFrame_HMDDataPacket *hmd_data_frame =
        data_frame->mutable_hmd_data_packet();

    hmd_data_frame->set_hmd_id(hmd_view->getDeviceID());
    hmd_data_frame->set_sequence_num(hmd_view->m_sequence_number);
    hmd_data_frame->set_isconnected(hmd_view->getDevice()->getIsOpen());

    switch (hmd_view->getHMDDeviceType())
    {
    case CommonHMDState::Morpheus:
        {
            generate_morpheus_hmd_data_frame_for_stream(hmd_view, stream_info, data_frame);
        } break;
    default:
        assert(0 && "Unhandled HMD type");
    }

    data_frame->set_device_category(PSMoveProtocol::DeviceOutputDataFrame::HMD);
}

static void
init_filters_for_morpheus_hmd(
    const MorpheusHMD *morpheusHMD,
    OrientationFilter *orientation_filter,
    PositionFilter *position_filter)
{
    const MorpheusHMDConfig *psmove_config = morpheusHMD->getConfig();

    {
        // Setup the space the orientation filter operates in
        Eigen::Vector3f identityGravity = Eigen::Vector3f::Zero();
        Eigen::Vector3f identityMagnetometer = Eigen::Vector3f::Zero();
        Eigen::Matrix3f calibrationTransform = Eigen::Matrix3f::Identity();
        Eigen::Matrix3f sensorTransform = Eigen::Matrix3f::Identity();
        OrientationFilterSpace filterSpace(identityGravity, identityMagnetometer, calibrationTransform, sensorTransform);
        orientation_filter->setFilterSpace(filterSpace);

        // Filter the orientation based on a blend of the optical orientation + IMU
        orientation_filter->setFusionType(OrientationFilter::FusionTypeComplementaryOpticalARG);
    }

    {
        // Setup the space the position filter operates in
		Eigen::Vector3f identityGravity = Eigen::Vector3f(0.f, 1.f, 0.f);
		Eigen::Matrix3f calibrationTransform = *k_eigen_identity_pose_laying_flat;
		Eigen::Matrix3f sensorTransform = *k_eigen_sensor_transform_opengl;
		PositionFilterSpace filterSpace(identityGravity, calibrationTransform, sensorTransform);

        position_filter->setFilterSpace(filterSpace);
        position_filter->setFusionType(PositionFilter::FusionTypeComplimentaryOpticalIMU);
    }
}

static void
update_filters_for_morpheus_hmd(
    const MorpheusHMD *morpheusHMD,
    const MorpheusHMDState *morpheusHMDState,
	const float delta_time,
	const HMDOpticalPoseEstimation *poseEstimation,
    OrientationFilter *orientationFilter,
    PositionFilter *position_filter)
{
    const MorpheusHMDConfig *config = morpheusHMD->getConfig();

	Eigen::Quaternionf orientationFrames[2] = { Eigen::Quaternionf::Identity(), Eigen::Quaternionf::Identity() };

	// Update the orientation filter
	if (orientationFilter != nullptr)
	{
		OrientationSensorPacket sensorPacket;

		sensorPacket.magnetometer = Eigen::Vector3f::Zero();

		// Each state update contains two readings (one earlier and one later) of accelerometer and gyro data
		for (int frame = 0; frame < 2; ++frame)
		{
			sensorPacket.orientation = orientationFilter->getOrientation();
			sensorPacket.orientation_source = OrientationSource_PreviousFrame;
			sensorPacket.orientation_quality = -1.f; // not relevant for previous frame

			sensorPacket.accelerometer =
				Eigen::Vector3f(
					morpheusHMDState->CalibratedAccel[frame][0],
					morpheusHMDState->CalibratedAccel[frame][1],
					morpheusHMDState->CalibratedAccel[frame][2]);
			sensorPacket.gyroscope =
				Eigen::Vector3f(
					morpheusHMDState->CalibratedGyro[frame][0],
					morpheusHMDState->CalibratedGyro[frame][1],
					morpheusHMDState->CalibratedGyro[frame][2]);

			// Update the orientation filter using the sensor packet.
			// NOTE: The magnetometer reading is the same for both sensor readings.
			orientationFilter->update(delta_time / 2.f, sensorPacket);
		}
	}

	// Update the position filter
	if (position_filter != nullptr)
	{
		PositionSensorPacket sensorPacket;

		if (poseEstimation->bCurrentlyTracking)
		{
			sensorPacket.world_position =
				Eigen::Vector3f(
					poseEstimation->position.x,
					poseEstimation->position.y,
					poseEstimation->position.z);
			sensorPacket.position_source = PositionSource_Optical;
			sensorPacket.position_quality =
				clampf01(
					safe_divide_with_default(
						poseEstimation->projection.screen_area - config->min_position_quality_screen_area,
						config->max_position_quality_screen_area - config->min_position_quality_screen_area,
						1.f));
		}
		else
		{
			sensorPacket.world_position = position_filter->getPosition();
			sensorPacket.position_source = PositionSource_PreviousFrame;
			sensorPacket.position_quality = -1.f; // not relevant for previous frame
		}

		switch (position_filter->getFusionType())
		{
		case PositionFilter::FusionTypeNone:
		case PositionFilter::FusionTypePassThru:
		case PositionFilter::FusionTypeLowPassOptical:
		case PositionFilter::FusionTypeLowPassExponential:
		{
			// All other filter types don't use transformed IMU data
			sensorPacket.world_orientation = Eigen::Quaternionf::Identity();
			sensorPacket.accelerometer = Eigen::Vector3f::Zero();

			// Update the orientation filter using the sensor packet.
			position_filter->update(delta_time, sensorPacket);
		} break;
		case PositionFilter::FusionTypeLowPassIMU:
		case PositionFilter::FusionTypeComplimentaryOpticalIMU:
		{
			// Each state update contains two readings (one earlier and one later) of accelerometer data
			for (int frame = 0; frame < 2; ++frame)
			{
				// Use the latest estimated orientation for the frame
				sensorPacket.world_orientation = orientationFrames[frame];

				// The filter will use the current orientation and the identity gravity direction
				// to subtract out gravity and get acceleration of the controller in world space
				sensorPacket.accelerometer =
					Eigen::Vector3f(
						morpheusHMDState->CalibratedAccel[frame][0],
						morpheusHMDState->CalibratedAccel[frame][1],
						morpheusHMDState->CalibratedAccel[frame][2]);

				// Update the orientation filter using the sensor packet for each frame.
				position_filter->update(delta_time / 2.f, sensorPacket);
			}
		} break;
		default:
			assert(0 && "unreachable");
		}
	}
}

static void generate_morpheus_hmd_data_frame_for_stream(
    const ServerHMDView *hmd_view,
    const HMDStreamInfo *stream_info,
    DeviceOutputDataFramePtr &data_frame)
{
    const MorpheusHMD *morpheus_hmd = hmd_view->castCheckedConst<MorpheusHMD>();
    const MorpheusHMDConfig *morpheus_config = morpheus_hmd->getConfig();
	const OrientationFilter *orientation_filter = hmd_view->getOrientationFilter();
	const PositionFilter *position_filter = hmd_view->getPositionFilter();
    const CommonHMDState *hmd_state = hmd_view->getState();
    const CommonDevicePose hmd_pose = hmd_view->getFilteredPose();

    PSMoveProtocol::DeviceOutputDataFrame_HMDDataPacket *hmd_data_frame = data_frame->mutable_hmd_data_packet();

    if (hmd_state != nullptr)
    {
        assert(hmd_state->DeviceType == CommonDeviceState::Morpheus);
        const MorpheusHMDState * morpheus_hmd_state = static_cast<const MorpheusHMDState *>(hmd_state);

        PSMoveProtocol::DeviceOutputDataFrame_HMDDataPacket_MorpheusState* morpheus_data_frame = 
            hmd_data_frame->mutable_morpheus_state();

		morpheus_data_frame->set_iscurrentlytracking(hmd_view->getIsCurrentlyTracking());
		morpheus_data_frame->set_istrackingenabled(hmd_view->getIsTrackingEnabled());
		morpheus_data_frame->set_isorientationvalid(orientation_filter->getIsFusionStateValid());
		morpheus_data_frame->set_ispositionvalid(position_filter->getIsFusionStateValid());

		morpheus_data_frame->mutable_orientation()->set_w(hmd_pose.Orientation.w);
		morpheus_data_frame->mutable_orientation()->set_x(hmd_pose.Orientation.x);
		morpheus_data_frame->mutable_orientation()->set_y(hmd_pose.Orientation.y);
		morpheus_data_frame->mutable_orientation()->set_z(hmd_pose.Orientation.z);

		if (stream_info->include_position_data)
		{
			morpheus_data_frame->mutable_position()->set_x(hmd_pose.Position.x);
			morpheus_data_frame->mutable_position()->set_y(hmd_pose.Position.y);
			morpheus_data_frame->mutable_position()->set_z(hmd_pose.Position.z);
		}
		else
		{
			morpheus_data_frame->mutable_position()->set_x(0);
			morpheus_data_frame->mutable_position()->set_y(0);
			morpheus_data_frame->mutable_position()->set_z(0);
		}

		// If requested, get the raw sensor data for the hmd
		if (stream_info->include_physics_data)
		{
			const CommonDevicePhysics hmd_physics = hmd_view->getFilteredPhysics();
			auto *physics_data = morpheus_data_frame->mutable_physics_data();

			physics_data->mutable_velocity()->set_i(hmd_physics.Velocity.i);
			physics_data->mutable_velocity()->set_j(hmd_physics.Velocity.j);
			physics_data->mutable_velocity()->set_k(hmd_physics.Velocity.k);

			physics_data->mutable_acceleration()->set_i(hmd_physics.Acceleration.i);
			physics_data->mutable_acceleration()->set_j(hmd_physics.Acceleration.j);
			physics_data->mutable_acceleration()->set_k(hmd_physics.Acceleration.k);

			physics_data->mutable_angular_velocity()->set_i(hmd_physics.AngularVelocity.i);
			physics_data->mutable_angular_velocity()->set_j(hmd_physics.AngularVelocity.j);
			physics_data->mutable_angular_velocity()->set_k(hmd_physics.AngularVelocity.k);

			physics_data->mutable_angular_acceleration()->set_i(hmd_physics.AngularAcceleration.i);
			physics_data->mutable_angular_acceleration()->set_j(hmd_physics.AngularAcceleration.j);
			physics_data->mutable_angular_acceleration()->set_k(hmd_physics.AngularAcceleration.k);
		}

        // If requested, get the raw sensor data for the hmd
        if (stream_info->include_raw_sensor_data)
        {
            PSMoveProtocol::DeviceOutputDataFrame_HMDDataPacket_MorpheusState_RawSensorData *raw_sensor_data =
                morpheus_data_frame->mutable_raw_sensor_data();

			// Two frames: [[ax0, ay0, az0], [ax1, ay1, az1]] 
			// Take the most recent frame: [ax1, ay1, az1]
			raw_sensor_data->mutable_accelerometer()->set_i(morpheus_hmd_state->RawAccel[1][0]);
			raw_sensor_data->mutable_accelerometer()->set_j(morpheus_hmd_state->RawAccel[1][1]);
			raw_sensor_data->mutable_accelerometer()->set_k(morpheus_hmd_state->RawAccel[1][2]);

			// Two frames: [[wx0, wy0, wz0], [wx1, wy1, wz1]] 
			// Take the most recent frame: [wx1, wy1, wz1]
			raw_sensor_data->mutable_gyroscope()->set_i(morpheus_hmd_state->RawGyro[1][0]);
			raw_sensor_data->mutable_gyroscope()->set_j(morpheus_hmd_state->RawGyro[1][1]);
			raw_sensor_data->mutable_gyroscope()->set_k(morpheus_hmd_state->RawGyro[1][2]);
        }

		// If requested, get the raw sensor data for the hmd
		if (stream_info->include_calibrated_sensor_data)
		{
			PSMoveProtocol::DeviceOutputDataFrame_HMDDataPacket_MorpheusState_CalibratedSensorData *calibrated_sensor_data =
				morpheus_data_frame->mutable_calibrated_sensor_data();

			// Two frames: [[ax0, ay0, az0], [ax1, ay1, az1]] 
			// Take the most recent frame: [ax1, ay1, az1]
			calibrated_sensor_data->mutable_accelerometer()->set_i(morpheus_hmd_state->CalibratedAccel[1][0]);
			calibrated_sensor_data->mutable_accelerometer()->set_j(morpheus_hmd_state->CalibratedAccel[1][1]);
			calibrated_sensor_data->mutable_accelerometer()->set_k(morpheus_hmd_state->CalibratedAccel[1][2]);

			// Two frames: [[wx0, wy0, wz0], [wx1, wy1, wz1]] 
			// Take the most recent frame: [wx1, wy1, wz1]
			calibrated_sensor_data->mutable_gyroscope()->set_i(morpheus_hmd_state->CalibratedGyro[1][0]);
			calibrated_sensor_data->mutable_gyroscope()->set_j(morpheus_hmd_state->CalibratedGyro[1][1]);
			calibrated_sensor_data->mutable_gyroscope()->set_k(morpheus_hmd_state->CalibratedGyro[1][2]);
		}

		// If requested, get the raw tracker data for the controller
		if (stream_info->include_raw_tracker_data)
		{
			auto *raw_tracker_data = morpheus_data_frame->mutable_raw_tracker_data();
			int valid_tracker_count = 0;

			for (int trackerId = 0; trackerId < TrackerManager::k_max_devices; ++trackerId)
			{
				const HMDOpticalPoseEstimation *positionEstimate = hmd_view->getTrackerPoseEstimate(trackerId);

				if (positionEstimate != nullptr && positionEstimate->bCurrentlyTracking)
				{
					const CommonDevicePosition &trackerRelativePosition = positionEstimate->position;
					const ServerTrackerViewPtr tracker_view = DeviceManager::getInstance()->getTrackerViewPtr(trackerId);

					// Project the 3d camera position back onto the tracker screen
					{
						const CommonDeviceScreenLocation trackerScreenLocation =
							tracker_view->projectTrackerRelativePosition(&trackerRelativePosition);
						PSMoveProtocol::Pixel *pixel = raw_tracker_data->add_screen_locations();

						pixel->set_x(trackerScreenLocation.x);
						pixel->set_y(trackerScreenLocation.y);
					}

					// Add the tracker relative 3d position
					{
						PSMoveProtocol::Position *position = raw_tracker_data->add_relative_positions();

						position->set_x(trackerRelativePosition.x);
						position->set_y(trackerRelativePosition.y);
						position->set_z(trackerRelativePosition.z);
					}

					// Add the tracker relative projection shapes
					{
						const CommonDeviceTrackingProjection &trackerRelativeProjection =
							positionEstimate->projection;

						assert(trackerRelativeProjection.shape_type == eCommonTrackingProjectionType::ProjectionType_Points);
						PSMoveProtocol::Polygon *polygon = raw_tracker_data->add_projected_point_cloud();

						for (int vert_index = 0; vert_index < trackerRelativeProjection.shape.points.point_count; ++vert_index)
						{
							PSMoveProtocol::Pixel *pixel = polygon->add_vertices();

							pixel->set_x(trackerRelativeProjection.shape.points.point[vert_index].x);
							pixel->set_y(trackerRelativeProjection.shape.points.point[vert_index].y);
						}
					}

					raw_tracker_data->add_tracker_ids(trackerId);
					++valid_tracker_count;
				}
			}

			raw_tracker_data->set_valid_tracker_count(valid_tracker_count);
		}
    }

    hmd_data_frame->set_hmd_type(PSMoveProtocol::Morpheus);
}

static Eigen::Vector3f CommonDevicePosition_to_EigenVector3f(const CommonDevicePosition &p)
{
    return Eigen::Vector3f(p.x, p.y, p.z);
}

static Eigen::Vector3f CommonDeviceVector_to_EigenVector3f(const CommonDeviceVector &v)
{
    return Eigen::Vector3f(v.i, v.j, v.k);
}

static Eigen::Quaternionf CommonDeviceQuaternion_to_EigenQuaternionf(const CommonDeviceQuaternion &q)
{
    return Eigen::Quaternionf(q.w, q.x, q.y, q.z);
}

static CommonDevicePosition EigenVector3f_to_CommonDevicePosition(const Eigen::Vector3f &p)
{
    CommonDevicePosition result;

    result.x = p.x();
    result.y = p.y();
    result.z = p.z();

    return result;
}

static CommonDeviceQuaternion EigenQuaternionf_to_CommonDeviceQuaternion(const Eigen::Quaternionf &q)
{
    CommonDeviceQuaternion result;

    result.w = q.w();
    result.x = q.x();
    result.y = q.y();
    result.z = q.z();

    return result;
}

