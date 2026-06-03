#include "SwitchAppActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "network/OtaBootSwitch.h"

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SwitchAppActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  requestUpdate();
}

void SwitchAppActivity::onExit() { Activity::onExit(); }

void SwitchAppActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SWITCH_APP));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_SWITCH_APP_WARNING), true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SWITCH_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SWITCHING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SWITCHING_APP));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_APP_SWITCHED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_SWITCH_APP_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void SwitchAppActivity::switchApp() {
  LOG_DBG("SWITCH_APP", "Switching App...");

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("SWITCH_APP", "no next-update partition");
    state = FAILED;
    return;
  }

  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("SWITCH_APP", "otadata switch failed");
    state = FAILED;
    return;
  }

  state = SUCCESS;

  esp_restart();

  return;
}

void SwitchAppActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("SWITCH_APP", "User confirmed, starting app switch");
      {
        RenderLock lock(*this);
        state = SWITCHING;
      }
      requestUpdateAndWait();

      switchApp();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("SWITCH_APP", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    requestUpdateAndWait();

    if (state == SUCCESS) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        ESP.restart();
      }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
