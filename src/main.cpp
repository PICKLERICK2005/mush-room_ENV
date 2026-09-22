#include <Arduino.h>
#include <Wire.h>
#include <M5UnitENV.h>

SHT3X sht30;
QMP6988 qmp6988;

constexpr int SDA_PIN = 2;
constexpr int SCL_PIN = 1;

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("MUSHROOM ENV LOGGER");
    Serial.println("===================");

    if (!qmp6988.begin(&Wire, QMP6988_SLAVE_ADDRESS_L,
                       SDA_PIN, SCL_PIN, 400000U)) {
        Serial.println("ERROR: QMP6988 not detected!");
        while (true) delay(1000);
    }

    if (!sht30.begin(&Wire, SHT3X_I2C_ADDR,
                     SDA_PIN, SCL_PIN, 400000U)) {
        Serial.println("ERROR: SHT30 not detected!");
        while (true) delay(1000);
    }

    Serial.println("ENV III detected!");
    Serial.println();
    Serial.println("uptime_s,temp_c,rh_pct,pressure_hpa");
}

void loop() {
    const bool tempHumidityOK = sht30.update();
    const bool pressureOK = qmp6988.update();

    if (tempHumidityOK && pressureOK) {
        Serial.printf(
            "%.1f,%.2f,%.2f,%.2f\n",
            millis() / 1000.0,
            sht30.cTemp,
            sht30.humidity,
            qmp6988.pressure / 100.0
        );
    } else {
        Serial.println("Sensor read failed!");
    }

    delay(5000);
}
