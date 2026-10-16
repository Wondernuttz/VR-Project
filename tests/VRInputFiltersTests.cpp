#include "PrismaUI/VRInputFilters.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
	using PrismaUI::VRInput::AdaptivePoseFilter;
	using PrismaUI::VRInput::AccumulateScrollDelta;
	using PrismaUI::VRInput::ApplyDeadzone;
	using PrismaUI::VRInput::CaptureGrabAnchor;
	using PrismaUI::VRInput::Length;
	using PrismaUI::VRInput::PoseFilterConfig;
	using PrismaUI::VRInput::ReanchorGrabAtCurrentPosition;
	using PrismaUI::VRInput::ResolveGrabbedPanelCenter;
	using PrismaUI::VRInput::ShouldActivateScrollCapture;
	using PrismaUI::VRInput::Vec3;

	int failures = 0;

	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	bool NearlyEqual(float left, float right, float tolerance = 1.0e-5f)
	{
		return std::fabs(left - right) <= tolerance;
	}

	bool NearlyEqual(const Vec3& left, const Vec3& right, float tolerance = 1.0e-5f)
	{
		return NearlyEqual(left.x, right.x, tolerance) &&
		       NearlyEqual(left.y, right.y, tolerance) &&
		       NearlyEqual(left.z, right.z, tolerance);
	}

	void TestGrabAnchorPreservesOffCenterHit()
	{
		const Vec3 panelCenter{1.25f, 1.80f, -2.40f};
		const Vec3 rayOrigin{0.10f, 1.20f, 0.30f};
		const Vec3 rayDirection{0.0f, 0.0f, -1.0f};
		const auto anchor = CaptureGrabAnchor(panelCenter, rayOrigin, rayDirection, 2.0f);
		Expect(NearlyEqual(ResolveGrabbedPanelCenter(rayOrigin, rayDirection, anchor), panelCenter),
		       "off-center grab does not snap on its first frame");

		const Vec3 movedOrigin{0.25f, 1.35f, 0.10f};
		const Vec3 expectedMovedCenter{1.40f, 1.95f, -2.60f};
		Expect(NearlyEqual(ResolveGrabbedPanelCenter(movedOrigin, rayDirection, anchor),
		                   expectedMovedCenter),
		       "grabbed point follows controller motion with its original offset");
	}

	void TestResizeReanchorIsContinuous()
	{
		const Vec3 resizedCenter{-0.70f, 1.55f, -1.25f};
		const Vec3 rayOrigin{-0.25f, 1.10f, 0.20f};
		const Vec3 rayDirection{0.0f, 0.0f, -1.0f};
		const auto anchor = ReanchorGrabAtCurrentPosition(
			resizedCenter, rayOrigin, rayDirection, 0.2f, 5.0f);
		Expect(NearlyEqual(ResolveGrabbedPanelCenter(rayOrigin, rayDirection, anchor), resizedCenter),
		       "leaving two-hand resize preserves the current panel center");
	}

	void TestScrollCaptureRequiresDeliberateSameHandHold()
	{
		Expect(!ShouldActivateScrollCapture(0.499f, true, false, false),
		       "a sub-half-second hold remains a normal click");
		Expect(ShouldActivateScrollCapture(0.5f, true, false, false),
		       "a half-second hold captures the pane");
		Expect(!ShouldActivateScrollCapture(2.0f, false, false, false),
		       "holding outside scrollable content cannot enter scroll capture");
		Expect(!ShouldActivateScrollCapture(2.0f, true, true, false),
		       "an unresolved DOM probe cannot capture the wrong pane");
		Expect(!ShouldActivateScrollCapture(2.0f, true, false, true),
		       "a direct-drag control keeps its native drag interaction");
	}

	void TestDeadzone()
	{
		Expect(ApplyDeadzone(0.10f, 0.18f) == 0.0f, "deadzone suppresses small positive input");
		Expect(ApplyDeadzone(-0.18f, 0.18f) == 0.0f, "deadzone includes its negative boundary");
		Expect(std::fabs(ApplyDeadzone(1.0f, 0.18f) - 1.0f) < 1.0e-6f,
		       "deadzone preserves full positive deflection");
		Expect(std::fabs(ApplyDeadzone(-1.0f, 0.18f) + 1.0f) < 1.0e-6f,
		       "deadzone preserves full negative deflection");
	}

	void TestDisabledFilterIsExact()
	{
		AdaptivePoseFilter filter;
		PoseFilterConfig config;
		config.enabled = false;
		Vec3 position{};
		Vec3 direction{};
		filter.Update({1.0f, 2.0f, 3.0f}, {0.0f, 2.0f, 0.0f}, 1.0f / 90.0f,
		              config, position, direction);
		Expect(position.x == 1.0f && position.y == 2.0f && position.z == 3.0f,
		       "disabled filter preserves raw position");
		Expect(std::fabs(direction.y - 1.0f) < 1.0e-6f,
		       "disabled filter normalizes raw direction");
	}

	void TestDirectionStaysNormalized()
	{
		AdaptivePoseFilter filter;
		PoseFilterConfig config;
		Vec3 position{};
		Vec3 direction{};
		filter.Update({}, {0.0f, 0.0f, -1.0f}, 1.0f / 90.0f, config, position, direction);
		for (int i = 0; i < 60; ++i)
			filter.Update({}, {0.15f, 0.0f, -0.98f}, 1.0f / 90.0f, config, position, direction);
		Expect(std::fabs(Length(direction) - 1.0f) < 1.0e-5f,
		       "filtered direction remains normalized");
	}

	void TestDiscontinuitySnaps()
	{
		AdaptivePoseFilter filter;
		PoseFilterConfig config;
		Vec3 position{};
		Vec3 direction{};
		filter.Update({}, {0.0f, 0.0f, -1.0f}, 1.0f / 90.0f, config, position, direction);
		filter.Update({0.50f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1.0f / 90.0f,
		              config, position, direction);
		Expect(std::fabs(position.x - 0.50f) < 1.0e-6f, "large positional discontinuity snaps");
		Expect(std::fabs(direction.x - 1.0f) < 1.0e-6f, "large angular discontinuity snaps");
	}

	void TestNonFinitePoseCannotPoisonFilter()
	{
		AdaptivePoseFilter filter;
		PoseFilterConfig config;
		Vec3 position{};
		Vec3 direction{};
		filter.Update({}, {0.0f, 0.0f, -1.0f}, 1.0f / 90.0f, config, position, direction);
		const float nan = std::numeric_limits<float>::quiet_NaN();
		filter.Update({nan, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 1.0f / 90.0f,
		              config, position, direction);
		Expect(!filter.IsInitialized(), "non-finite pose resets the filter");
		Expect(std::isfinite(position.x) && std::isfinite(direction.z),
		       "non-finite pose never escapes the filter");
		filter.Update({1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}, 1.0f / 90.0f,
		              config, position, direction);
		Expect(filter.IsInitialized() && position.x == 1.0f && direction.y == 1.0f,
		       "valid tracking reacquires cleanly after a non-finite sample");
	}

	void TestFrameRateIndependence()
	{
		PoseFilterConfig config;
		config.directionBeta = 0.0f;
		AdaptivePoseFilter filter60;
		AdaptivePoseFilter filter90;
		Vec3 p60{}, d60{}, p90{}, d90{};
		filter60.Update({}, {0.0f, 0.0f, -1.0f}, 1.0f / 60.0f, config, p60, d60);
		filter90.Update({}, {0.0f, 0.0f, -1.0f}, 1.0f / 90.0f, config, p90, d90);
		for (int i = 0; i < 60; ++i)
			filter60.Update({}, {0.25f, 0.0f, -1.0f}, 1.0f / 60.0f, config, p60, d60);
		for (int i = 0; i < 90; ++i)
			filter90.Update({}, {0.25f, 0.0f, -1.0f}, 1.0f / 90.0f, config, p90, d90);
		Expect(std::fabs(d60.x - d90.x) < 0.002f,
		       "one-second response is stable across 60 and 90 Hz");
	}

	int IntegrateScrollAtRate(int framesPerSecond)
	{
		float remainder = 0.0f;
		int pixels = 0;
		for (int i = 0; i < framesPerSecond; ++i) {
			pixels += AccumulateScrollDelta(0.75f, 1.0f / static_cast<float>(framesPerSecond),
			                                    0.18f, 1000.0f, remainder);
		}
		return pixels;
	}

	void TestScrollRateIndependence()
	{
		const int pixels60 = IntegrateScrollAtRate(60);
		const int pixels90 = IntegrateScrollAtRate(90);
		const int pixels120 = IntegrateScrollAtRate(120);
		Expect(std::abs(pixels60 - pixels90) <= 1, "scroll distance matches at 60 and 90 Hz");
		Expect(std::abs(pixels90 - pixels120) <= 1, "scroll distance matches at 90 and 120 Hz");
		float remainder = 0.75f;
		Expect(AccumulateScrollDelta(0.10f, 1.0f / 90.0f, 0.18f, 1000.0f, remainder) == 0 &&
		       remainder == 0.0f, "deadzone clears fractional scroll carry");
	}
}

int main()
{
	TestGrabAnchorPreservesOffCenterHit();
	TestResizeReanchorIsContinuous();
	TestScrollCaptureRequiresDeliberateSameHandHold();
	TestDeadzone();
	TestDisabledFilterIsExact();
	TestDirectionStaysNormalized();
	TestDiscontinuitySnaps();
	TestNonFinitePoseCannotPoisonFilter();
	TestFrameRateIndependence();
	TestScrollRateIndependence();
	if (failures != 0)
		return 1;
	std::cout << "VR input filter tests passed\n";
	return 0;
}
