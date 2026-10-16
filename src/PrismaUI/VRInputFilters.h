#pragma once

#include <algorithm>
#include <cmath>

namespace PrismaUI::VRInput {

	struct Vec3 {
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	struct GrabAnchor {
		float distance = 1.0f;
		Vec3 offset{};
	};

	struct PoseFilterConfig {
		bool enabled = true;
		float directionMinCutoffHz = 4.0f;
		float directionBeta = 0.35f;
		float positionMinCutoffHz = 6.0f;
		float positionBeta = 12.0f;
		float derivativeCutoffHz = 1.0f;
		float snapAngleDegrees = 60.0f;
		float snapDistanceMeters = 0.35f;
		float maxGapSeconds = 0.25f;
	};

	inline float Clamp(float value, float low, float high) noexcept
	{
		return std::clamp(value, low, high);
	}

	inline float Dot(const Vec3& a, const Vec3& b) noexcept
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	inline float Length(const Vec3& value) noexcept
	{
		return std::sqrt(Dot(value, value));
	}

	inline float Distance(const Vec3& a, const Vec3& b) noexcept
	{
		return Length({a.x - b.x, a.y - b.y, a.z - b.z});
	}

	inline bool IsFinite(const Vec3& value) noexcept
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	inline Vec3 Normalize(const Vec3& value, const Vec3& fallback = {0.0f, 0.0f, -1.0f}) noexcept
	{
		const float length = Length(value);
		if (!std::isfinite(length) || length < 1.0e-6f)
			return fallback;
		return {value.x / length, value.y / length, value.z / length};
	}

	inline Vec3 Lerp(const Vec3& a, const Vec3& b, float alpha) noexcept
	{
		return {
			a.x + (b.x - a.x) * alpha,
			a.y + (b.y - a.y) * alpha,
			a.z + (b.z - a.z) * alpha
		};
	}

	inline Vec3 PointOnRay(const Vec3& origin, const Vec3& direction, float distance) noexcept
	{
		return {
			origin.x + direction.x * distance,
			origin.y + direction.y * distance,
			origin.z + direction.z * distance
		};
	}

	inline GrabAnchor CaptureGrabAnchor(const Vec3& panelCenter, const Vec3& rayOrigin,
	                                   const Vec3& rayDirection, float hitDistance) noexcept
	{
		const Vec3 rayPoint = PointOnRay(rayOrigin, rayDirection, hitDistance);
		return {
			hitDistance,
			{panelCenter.x - rayPoint.x, panelCenter.y - rayPoint.y, panelCenter.z - rayPoint.z}
		};
	}

	inline GrabAnchor ReanchorGrabAtCurrentPosition(const Vec3& panelCenter, const Vec3& rayOrigin,
	                                               const Vec3& rayDirection, float minDistance,
	                                               float maxDistance) noexcept
	{
		const Vec3 centerDelta{
			panelCenter.x - rayOrigin.x,
			panelCenter.y - rayOrigin.y,
			panelCenter.z - rayOrigin.z
		};
		const float distance = Clamp(Dot(centerDelta, rayDirection), minDistance, maxDistance);
		return CaptureGrabAnchor(panelCenter, rayOrigin, rayDirection, distance);
	}

	inline Vec3 ResolveGrabbedPanelCenter(const Vec3& rayOrigin, const Vec3& rayDirection,
	                                     const GrabAnchor& anchor) noexcept
	{
		const Vec3 rayPoint = PointOnRay(rayOrigin, rayDirection, anchor.distance);
		return {
			rayPoint.x + anchor.offset.x,
			rayPoint.y + anchor.offset.y,
			rayPoint.z + anchor.offset.z
		};
	}

	inline bool ShouldActivateScrollCapture(float heldSeconds,
	                                       bool hasCapturedScrollTarget,
	                                       bool probePending,
	                                       bool directDragActive,
	                                       float holdThresholdSeconds = 0.5f) noexcept
	{
		return std::isfinite(heldSeconds) && std::isfinite(holdThresholdSeconds) &&
		       heldSeconds >= holdThresholdSeconds && hasCapturedScrollTarget &&
		       !probePending && !directDragActive;
	}

	inline float LowPassAlpha(float deltaSeconds, float cutoffHz) noexcept
	{
		if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
			return 1.0f;
		cutoffHz = std::max(cutoffHz, 0.001f);
		constexpr float twoPi = 6.28318530717958647692f;
		const float timeConstant = 1.0f / (twoPi * cutoffHz);
		return Clamp(deltaSeconds / (deltaSeconds + timeConstant), 0.0f, 1.0f);
	}

	inline float ApplyDeadzone(float value, float deadzone) noexcept
	{
		deadzone = Clamp(deadzone, 0.0f, 0.95f);
		const float magnitude = std::fabs(value);
		if (magnitude <= deadzone)
			return 0.0f;
		const float normalized = (magnitude - deadzone) / (1.0f - deadzone);
		return std::copysign(Clamp(normalized, 0.0f, 1.0f), value);
	}

	inline int AccumulateScrollDelta(float axis, float deltaSeconds, float deadzone,
	                                 float pixelsPerSecond, float& remainder) noexcept
	{
		if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
			return 0;
		}
		const float filteredAxis = ApplyDeadzone(axis, deadzone);
		if (filteredAxis == 0.0f) {
			remainder = 0.0f;
			return 0;
		}
		deltaSeconds = Clamp(deltaSeconds, 0.0f, 0.05f);
		pixelsPerSecond = Clamp(pixelsPerSecond, 0.0f, 10000.0f);
		remainder += filteredAxis * pixelsPerSecond * deltaSeconds;
		const int wholePixels = static_cast<int>(std::trunc(remainder));
		remainder -= static_cast<float>(wholePixels);
		return wholePixels;
	}

	class AdaptivePoseFilter {
	public:
		void Reset() noexcept
		{
			initialized_ = false;
			filteredAngularSpeed_ = 0.0f;
			filteredLinearSpeed_ = 0.0f;
		}

		void Update(const Vec3& rawPosition, const Vec3& rawDirection, float deltaSeconds,
		            const PoseFilterConfig& config, Vec3& outputPosition, Vec3& outputDirection) noexcept
		{
			if (!IsFinite(rawPosition) || !IsFinite(rawDirection)) {
				Reset();
				outputPosition = {};
				outputDirection = {0.0f, 0.0f, -1.0f};
				return;
			}
			const Vec3 normalizedDirection = Normalize(rawDirection);
			if (!config.enabled) {
				ResetTo(rawPosition, normalizedDirection);
				outputPosition = rawPosition;
				outputDirection = normalizedDirection;
				return;
			}

			const bool invalidGap = !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f ||
			                        deltaSeconds > config.maxGapSeconds;
			if (!initialized_ || invalidGap) {
				ResetTo(rawPosition, normalizedDirection);
				outputPosition = filteredPosition_;
				outputDirection = filteredDirection_;
				return;
			}

			const float directionDot = Clamp(Dot(previousRawDirection_, normalizedDirection), -1.0f, 1.0f);
			const float angleRadians = std::acos(directionDot);
			const float angleDegrees = angleRadians * 57.29577951308232f;
			const float positionDelta = Distance(previousRawPosition_, rawPosition);
			if (angleDegrees >= config.snapAngleDegrees || positionDelta >= config.snapDistanceMeters) {
				ResetTo(rawPosition, normalizedDirection);
				outputPosition = filteredPosition_;
				outputDirection = filteredDirection_;
				return;
			}

			const float derivativeAlpha = LowPassAlpha(deltaSeconds, config.derivativeCutoffHz);
			const float angularSpeed = angleRadians / deltaSeconds;
			const float linearSpeed = positionDelta / deltaSeconds;
			filteredAngularSpeed_ += derivativeAlpha * (angularSpeed - filteredAngularSpeed_);
			filteredLinearSpeed_ += derivativeAlpha * (linearSpeed - filteredLinearSpeed_);

			const float directionCutoff = config.directionMinCutoffHz +
			                               config.directionBeta * std::fabs(filteredAngularSpeed_);
			const float positionCutoff = config.positionMinCutoffHz +
			                              config.positionBeta * std::fabs(filteredLinearSpeed_);
			const float directionAlpha = LowPassAlpha(deltaSeconds, directionCutoff);
			const float positionAlpha = LowPassAlpha(deltaSeconds, positionCutoff);

			filteredPosition_ = Lerp(filteredPosition_, rawPosition, positionAlpha);
			filteredDirection_ = Normalize(Lerp(filteredDirection_, normalizedDirection, directionAlpha),
			                                     normalizedDirection);
			previousRawPosition_ = rawPosition;
			previousRawDirection_ = normalizedDirection;

			outputPosition = filteredPosition_;
			outputDirection = filteredDirection_;
		}

		bool IsInitialized() const noexcept { return initialized_; }

	private:
		void ResetTo(const Vec3& position, const Vec3& direction) noexcept
		{
			initialized_ = true;
			filteredPosition_ = position;
			filteredDirection_ = direction;
			previousRawPosition_ = position;
			previousRawDirection_ = direction;
			filteredAngularSpeed_ = 0.0f;
			filteredLinearSpeed_ = 0.0f;
		}

		bool initialized_ = false;
		Vec3 filteredPosition_{};
		Vec3 filteredDirection_{0.0f, 0.0f, -1.0f};
		Vec3 previousRawPosition_{};
		Vec3 previousRawDirection_{0.0f, 0.0f, -1.0f};
		float filteredAngularSpeed_ = 0.0f;
		float filteredLinearSpeed_ = 0.0f;
	};

} // namespace PrismaUI::VRInput
