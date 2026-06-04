#include "KalmanFilter.h"

KalmanFilter::KalmanFilter(float initial_x, float initial_P, float Q, float R) {
    _x = initial_x;
    _cov = initial_P;
    _Q = Q;
    _R = R;
    _mutex = xSemaphoreCreateMutex();
}

KalmanFilter::~KalmanFilter() {
    if (_mutex != NULL) {
        vSemaphoreDelete(_mutex);
    }
}

float KalmanFilter::update(float measurement) {
    float result = 0;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // 1. Kalman Gain (K) kiszámítása
        float K = _cov / (_cov + _R);

        // 2. Becslés frissítése az új méréssel
        _x = _x + K * (measurement - _x);

        // 3. Kovariancia frissítése a következő lépéshez
        _cov = (1.0f - K) * _cov + _Q;

        result = _x;
        xSemaphoreGive(_mutex);
    }
    
    return result;
}

float KalmanFilter::getState() {
    float result = 0;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        result = _x;
        xSemaphoreGive(_mutex);
    }
    return result;
}

void KalmanFilter::setParameters(float Q, float R) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _Q = Q;
        _R = R;
        xSemaphoreGive(_mutex);
    }
}

void KalmanFilter::reset(float new_x, float uncertainty) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _x = new_x;
        _cov = uncertainty; // Újra "tanuló" fázisba helyezzük
        xSemaphoreGive(_mutex);
    }
}