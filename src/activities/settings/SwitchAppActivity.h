#pragma once

#include <functional>

#include "activities/Activity.h"

class SwitchAppActivity final : public Activity {
 public:
  explicit SwitchAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SwitchApp", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, SWITCHING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  void switchApp();
};
