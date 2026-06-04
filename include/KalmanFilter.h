#ifndef KALMAN_FILTER_H
#define KALMAN_FILTER_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class KalmanFilter {
private:
    float _x; // Becsült állapot
    float _cov; // Hiba kovariancia
    float _Q; // Folyamat zaj
    float _R; // Mérési zaj
    
    SemaphoreHandle_t _mutex;

public:
    /**
     * @param initial_x Kezdőérték
     * @param initial_P Kezdő hiba kovariancia (pl. 1.0)
     * @param Q Folyamat zaj (milyen gyorsan változik a rendszer, pl. 0.01)
     * @param R Mérési zaj (mennyire zajos a szenzor, pl. 0.1)
     */
    KalmanFilter(float initial_x, float initial_P, float Q, float R);
    ~KalmanFilter();

    // Új mérés feldolgozása és a szűrt érték visszaadása
    float update(float measurement);

    // Aktuális szűrt érték lekérése frissítés nélkül
    float getState();

    // Paraméterek menet közbeni módosítása (ha szükséges)
    void setParameters(float Q, float R);
	void reset(float new_x, float uncertainty = 1.0f);
};

#endif