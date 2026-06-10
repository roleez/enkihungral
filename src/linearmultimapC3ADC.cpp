#include <Arduino.h>

// Kalibrációs pontok a mért adatok alapján
// _in:  nyers ADC értékek (Y oszlop)
// _out: feszültségek voltban (X oszlop)

const uint8_t MAP_SIZE = 10;

static float adcPoints[MAP_SIZE] = {
    22.113f,
    585.995f,
    1238.33f,
    1791.15f,
    2377.15f,
    2974.2f,
    3328.01f,
    3737.1f,
    4079.85f,
    4090.91f
};

static float voltPoints[MAP_SIZE] = {
    0.140518f,
    0.634353f,
    1.16132f,
    1.6083f,
    2.10223f,
    2.56344f,
    2.78023f,
    2.98321f,
    3.12038f,
    3.12043f
};

float multiMap(float val, float* _in, float* _out, uint8_t size)
{
    if (val <= _in[0]) return _out[0];
    if (val >= _in[size-1]) return _out[size-1];

    uint8_t pos = 1;
    while (val > _in[pos]) pos++;

    if (val == _in[pos]) return _out[pos];

    return (val - _in[pos-1]) * (_out[pos] - _out[pos-1]) / (_in[pos] - _in[pos-1]) + _out[pos-1];
}

// Kényelmi wrapper: ADC értékből egyből voltot ad vissza
float adcToVoltage(float adcVal)
{
    return multiMap(adcVal, adcPoints, voltPoints, MAP_SIZE);
}