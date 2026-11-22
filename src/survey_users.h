// Who is using a radio survey (wifi_survey.h, ble_survey.h): how many screens, when the last
// one left, and the survey's task, which the first user starts and every change wakes. The task
// stops the radio its kLingerMs after the last user goes (so moving between screens doesn't
// cycle it), then ends itself, clearing `task`. Guarded by the survey's own mutex: call with it
// held.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace survey {

struct Users {
  int count = 0;
  uint32_t lastGoneMs = 0;  // millis() when count fell to 0
  TaskHandle_t task = nullptr;

  void wake() {
    if (task) xTaskNotifyGive(task);
  }
  // A user: the first starts the task (`run`, as `name`).
  void add(const char *name, TaskFunction_t run) {
    ++count;
    if (!task) xTaskCreate(run, name, 4096, nullptr, 1, &task);
    wake();
  }
  void remove() {
    if (count == 0) return;
    if (--count == 0) lastGoneMs = millis();
    wake();
  }
};

}  // namespace survey
