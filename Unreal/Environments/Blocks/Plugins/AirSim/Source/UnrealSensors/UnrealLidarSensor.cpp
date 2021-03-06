// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "UnrealLidarSensor.h"
#include "AirBlueprintLib.h"
#include "common/Common.hpp"
#include "NedTransform.h"
#include "DrawDebugHelpers.h"

// ctor
UnrealLidarSensor::UnrealLidarSensor(const AirSimSettings::LidarSetting& setting,
    AActor* actor, const NedTransform* ned_transform)
    : LidarSimple(setting), actor_(actor), ned_transform_(ned_transform)
{
    createLasers();
}

// initializes information based on lidar configuration
void UnrealLidarSensor::createLasers()
{
    msr::airlib::LidarSimpleParams params = getParams();

    const auto number_of_lasers = params.number_of_channels;

    if (number_of_lasers <= 0)
        return;

    // calculate verticle angle distance between each laser
    float delta_angle = 0;
    if (number_of_lasers > 1)
        delta_angle = (params.vertical_FOV_upper - (params.vertical_FOV_lower)) /
            static_cast<float>(number_of_lasers - 1);

    // store vertical angles for each laser
    laser_angles_.clear();
    for (auto i = 0u; i < number_of_lasers; ++i)
    {
        const float vertical_angle = params.vertical_FOV_upper - static_cast<float>(i) * delta_angle;
        laser_angles_.emplace_back(vertical_angle);
    }
}

// returns a point-cloud for the tick
// returns a point-cloud for the tick
void UnrealLidarSensor::getPointCloud(const msr::airlib::Pose& lidar_pose, const msr::airlib::Pose& vehicle_pose,
	const msr::airlib::TTimeDelta delta_time, msr::airlib::vector<msr::airlib::real_T>& point_cloud)
{
	point_cloud.clear();

	msr::airlib::LidarSimpleParams params = getParams();
	const auto number_of_lasers = params.number_of_channels;

	// cap the points to scan via ray-tracing; this is currently needed for car/Unreal tick scenarios
	// since SensorBase mechanism uses the elapsed clock time instead of the tick delta-time.
	constexpr float MAX_POINTS_IN_SCAN = 1e+5f;
	uint32 total_points_to_scan = FMath::RoundHalfFromZero(params.points_per_second * delta_time);
	if (total_points_to_scan > MAX_POINTS_IN_SCAN)
	{
		total_points_to_scan = MAX_POINTS_IN_SCAN;
		UAirBlueprintLib::LogMessageString("Lidar: ", "Capping number of points to scan", LogDebugLevel::Failure);
	}

	// calculate number of points needed for each laser/channel
	const uint32 points_to_scan_with_one_laser = FMath::RoundHalfFromZero(total_points_to_scan / float(number_of_lasers));
	if (points_to_scan_with_one_laser <= 0)
	{
		//UAirBlueprintLib::LogMessageString("Lidar: ", "No points requested this frame", LogDebugLevel::Failure);
		return;
	}

	// calculate needed angle/distance between each point
	const float angle_distance_of_tick = params.horizontal_rotation_frequency * 360.0f * delta_time;
	const float angle_distance_of_laser_measure = angle_distance_of_tick / points_to_scan_with_one_laser;

	// normalize FOV start/end
	const float laser_start = std::fmod(360.0f + params.horizontal_FOV_start, 360.0f);
	const float laser_end = std::fmod(360.0f + params.horizontal_FOV_end, 360.0f);

	// shoot lasers
	for (auto laser = 0u; laser < number_of_lasers; ++laser)
	{
		const float vertical_angle = laser_angles_[laser];

		for (auto i = 0u; i < points_to_scan_with_one_laser; ++i)
		{
			const float horizontal_angle = std::fmod(current_horizontal_angle_ + angle_distance_of_laser_measure * i, 360.0f);

			// check if the laser is outside the requested horizontal FOV
			if (!VectorMath::isAngleBetweenAngles(horizontal_angle, laser_start, laser_end))
				continue;

			Vector3r point;
			// shoot laser and get the impact point, if any
			if (shootLaser(lidar_pose, vehicle_pose, laser, 0, 15, params, point))

			//if (shootLaser(lidar_pose, vehicle_pose, laser, horizontal_angle, vertical_angle, params, point))
			{
				point_cloud.emplace_back(point.x());
				point_cloud.emplace_back(point.y());
				point_cloud.emplace_back(point.z());
			}
		}
	}

	current_horizontal_angle_ = std::fmod(current_horizontal_angle_ + angle_distance_of_tick, 360.0f);
	if (true && UAirBlueprintLib::IsInGameThread() && draw_new_point)
	{
		// Debug code for very specific cases.
		// Mostly shouldn't be needed. Use SimModeBase::drawLidarDebugPoints()
		FlushPersistentDebugLines(actor_->GetWorld());

		DrawDebugPoint(
			actor_->GetWorld(),
			singleshot_result.ImpactPoint,
			 15,                       //size
			FColor::Red,
			true,                    //persistent (never goes away)
			10                      //point leaves a trail on moving object
		);
		draw_new_point = false;
	}
	return;
}
msr::airlib::LidarData UnrealLidarSensor::doSingleLidarShotFrom(double horizontal_angle, double vertical_angle)
{
	msr::airlib::LidarSimpleParams params = getParams();
	const GroundTruth& ground_truth = getGroundTruth();

	const auto number_of_lasers = 1;// params.number_of_channels;
	msr::airlib::Pose lidar_pose = params.relative_pose + ground_truth.kinematics->pose;
	msr::airlib::Pose vehicle_pose = ground_truth.kinematics->pose;
	// start position
	Vector3r start = lidar_pose.position + vehicle_pose.position;

	// We need to compose rotations here rather than rotate a vector by a quaternion
	// Hence using coordOrientationAdd(..) rather than rotateQuaternion(..)

	// get ray quaternion in lidar frame (angles must be in radians)
	msr::airlib::Quaternionr ray_q_l = msr::airlib::VectorMath::toQuaternion(
		msr::airlib::Utils::degreesToRadians(vertical_angle),   //pitch - rotation around Y axis
		0,                                                      //roll  - rotation around X axis
		msr::airlib::Utils::degreesToRadians(horizontal_angle));//yaw   - rotation around Z axis

	// get ray quaternion in body frame
	msr::airlib::Quaternionr ray_q_b = VectorMath::coordOrientationAdd(ray_q_l, lidar_pose.orientation);

	// get ray quaternion in world frame
	msr::airlib::Quaternionr ray_q_w = VectorMath::coordOrientationAdd(ray_q_b, vehicle_pose.orientation);

	// get ray vector (end position)
	Vector3r endLocation = VectorMath::rotateVector(VectorMath::front(), ray_q_w, true) * params.range + start;
	std::vector<msr::airlib::real_T> end = { 0,0,0 };
	end[0] = endLocation[0];
	end[1] = endLocation[1];
	end[2] = endLocation[2];
	return doSingleLidarShot(end);
}
msr::airlib::LidarData UnrealLidarSensor::doSingleLidarShot(const std::vector<msr::airlib::real_T>& endLocation)
{
	UAirBlueprintLib::LogMessageString("Lidar: ", "SHOOTING NOW", LogDebugLevel::Failure);

	singleshot_output.point_cloud.clear();

	msr::airlib::LidarSimpleParams params = getParams();
	const GroundTruth& ground_truth = getGroundTruth();

	const auto number_of_lasers = 1;// params.number_of_channels;
	msr::airlib::Pose lidar_pose = params.relative_pose + ground_truth.kinematics->pose;
	msr::airlib::Pose vehicle_pose = ground_truth.kinematics->pose;
	// shoot lasers
	FHitResult hit_result = FHitResult(ForceInit);
	Vector3r start = lidar_pose.position + vehicle_pose.position;
	Vector3r end; //FIX ME TO BE IN THE CORRECT SPOT?????
	if (endLocation.size() < 3)
		return singleshot_output;

	end[0] = endLocation[0];
	end[1] = endLocation[1];
	end[2] = endLocation[2];

	singleshot_output.time_stamp = clock()->nowNanos();
	singleshot_output.pose = lidar_pose;
	singleshot_output.lidar = lidar_pose;
	singleshot_output.vehicle = vehicle_pose;

	bool is_hit = UAirBlueprintLib::GetObstacle(actor_, ned_transform_->fromLocalNed(start), ned_transform_->fromLocalNed(end), hit_result, actor_, ECC_Visibility);
	// decide the frame for the point-cloud
	Vector3r point;
	if (is_hit)
	{
		/*FViewport Viewport;
		EStereoscopicPass StereoPass;
		FSceneViewProjectionData ProjectionData;
		ULocalPlayer::GetProjectionData(Viewport, StereoPass, ProjectionData);*/
		//if (true && UAirBlueprintLib::IsInGameThread())
		{
			// Debug code for very specific cases.
			// Mostly shouldn't be needed. Use SimModeBase::drawLidarDebugPoints()
			/*FlushPersistentDebugLines(actor_->GetWorld());
			DrawDebugPoint(
				actor_->GetWorld(),
				hit_result.ImpactPoint,
				15,                       //size
				FColor::Red,
				true,                    //persistent (never goes away)
				10                      //point leaves a trail on moving object
			);*/
		}
		/*DrawDebugPoint(
			actor_->GetWorld(),
			hit_result.ImpactPoint,
			5,                       //size
			FColor::Green,
			true,                    //persistent (never goes away)
			0.1                      //point leaves a trail on moving object
		);*/
		singleshot_result = hit_result;
		draw_new_point = true;

		if (params.data_frame == AirSimSettings::kVehicleInertialFrame) {
			// current detault behavior; though it is probably not very useful.
			// not changing the default for now to maintain backwards-compat.
			point = ned_transform_->toLocalNed(hit_result.ImpactPoint);
		}
		else if (params.data_frame == AirSimSettings::kSensorLocalFrame) {
			// point in vehicle intertial frame
			Vector3r point_v_i = ned_transform_->toLocalNed(hit_result.ImpactPoint);

			// tranform to lidar frame
			point = VectorMath::transformToBodyFrame(point_v_i, lidar_pose + vehicle_pose, true);

			// The above should be same as first transforming to vehicle-body frame and then to lidar frame
			//    Vector3r point_v_b = VectorMath::transformToBodyFrame(point_v_i, vehicle_pose, true);
			//    point = VectorMath::transformToBodyFrame(point_v_b, lidar_pose, true);

			// On the client side, if it is needed to transform this data back to the world frame,
			// then do the equivalent of following,
			//     Vector3r point_w = VectorMath::transformToWorldFrame(point, lidar_pose + vehicle_pose, true);
			// See SimModeBase::drawLidarDebugPoints()

			// TODO: Optimization -- instead of doing this for every point, it should be possible to do this
			// for the point-cloud together? Need to look into matrix operations to do this together for all points.
		}
		else
			throw std::runtime_error("Unknown requested data frame");

		singleshot_output.point_cloud.emplace_back(point.x());
		singleshot_output.point_cloud.emplace_back(point.y());
		singleshot_output.point_cloud.emplace_back(point.z());
	}
	/*for (int laser = 0u; laser < number_of_lasers; ++laser)
	{
		const float vertical_angle = laser_angles_[laser];

		for (auto i = 0u; i < points_to_scan_with_one_laser; ++i)
		{
			const float horizontal_angle = std::fmod(current_horizontal_angle_ + angle_distance_of_laser_measure * i, 360.0f);

			// check if the laser is outside the requested horizontal FOV
			if (!VectorMath::isAngleBetweenAngles(horizontal_angle, laser_start, laser_end))
				continue;

			Vector3r point;
			// shoot laser and get the impact point, if any
			if (shootLaser(lidar_pose, vehicle_pose, laser, horizontal_angle, vertical_angle, params, point))
			{
				point_cloud.emplace_back(point.x());
				point_cloud.emplace_back(point.y());
				point_cloud.emplace_back(point.z());
			}
		}
	}*/

	//current_horizontal_angle_ = std::fmod(current_horizontal_angle_ + angle_distance_of_tick, 360.0f);

	return singleshot_output;
}
// simulate shooting a laser via Unreal ray-tracing.
bool UnrealLidarSensor::shootLaser(const msr::airlib::Pose& lidar_pose, const msr::airlib::Pose& vehicle_pose,
    const uint32 laser, const float horizontal_angle, const float vertical_angle, 
    const msr::airlib::LidarSimpleParams params, Vector3r &point)
{
    // start position
    Vector3r start = lidar_pose.position + vehicle_pose.position;

    // We need to compose rotations here rather than rotate a vector by a quaternion
    // Hence using coordOrientationAdd(..) rather than rotateQuaternion(..)

    // get ray quaternion in lidar frame (angles must be in radians)
    msr::airlib::Quaternionr ray_q_l = msr::airlib::VectorMath::toQuaternion(
        msr::airlib::Utils::degreesToRadians(vertical_angle),   //pitch - rotation around Y axis
        0,                                                      //roll  - rotation around X axis
        msr::airlib::Utils::degreesToRadians(horizontal_angle));//yaw   - rotation around Z axis

    // get ray quaternion in body frame
    msr::airlib::Quaternionr ray_q_b = VectorMath::coordOrientationAdd(ray_q_l, lidar_pose.orientation);

    // get ray quaternion in world frame
    msr::airlib::Quaternionr ray_q_w = VectorMath::coordOrientationAdd(ray_q_b, vehicle_pose.orientation);

    // get ray vector (end position)
    Vector3r end = VectorMath::rotateVector(VectorMath::front(), ray_q_w, true) * params.range + start;
   
    FHitResult hit_result = FHitResult(ForceInit);
    bool is_hit = UAirBlueprintLib::GetObstacle(actor_, ned_transform_->fromLocalNed(start), ned_transform_->fromLocalNed(end), hit_result, actor_, ECC_Visibility);

    if (is_hit)
    {
        if (false && UAirBlueprintLib::IsInGameThread())
        {
            // Debug code for very specific cases.
            // Mostly shouldn't be needed. Use SimModeBase::drawLidarDebugPoints()
            DrawDebugPoint(
                actor_->GetWorld(),
                hit_result.ImpactPoint,
                5,                       //size
                FColor::Red,
                true,                    //persistent (never goes away)
                0.1                      //point leaves a trail on moving object
            );
        }
        // decide the frame for the point-cloud
        if (params.data_frame == AirSimSettings::kVehicleInertialFrame) {
            // current detault behavior; though it is probably not very useful.
            // not changing the default for now to maintain backwards-compat.
            point = ned_transform_->toLocalNed(hit_result.ImpactPoint);
        }
        else if (params.data_frame == AirSimSettings::kSensorLocalFrame) {
            // point in vehicle intertial frame
            Vector3r point_v_i = ned_transform_->toLocalNed(hit_result.ImpactPoint);

            // tranform to lidar frame
            point = VectorMath::transformToBodyFrame(point_v_i, lidar_pose + vehicle_pose, true);

            // The above should be same as first transforming to vehicle-body frame and then to lidar frame
            //    Vector3r point_v_b = VectorMath::transformToBodyFrame(point_v_i, vehicle_pose, true);
            //    point = VectorMath::transformToBodyFrame(point_v_b, lidar_pose, true);

            // On the client side, if it is needed to transform this data back to the world frame,
            // then do the equivalent of following,
            //     Vector3r point_w = VectorMath::transformToWorldFrame(point, lidar_pose + vehicle_pose, true);
            // See SimModeBase::drawLidarDebugPoints()

            // TODO: Optimization -- instead of doing this for every point, it should be possible to do this
            // for the point-cloud together? Need to look into matrix operations to do this together for all points.
        }
        else 
            throw std::runtime_error("Unknown requested data frame");

        return true;
    }
    else 
    {
        return false;
    }
}
