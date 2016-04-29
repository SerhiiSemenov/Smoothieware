/*
      this file is part of smoothie (http://smoothieware.org/). the motion control part is heavily based on grbl (https://github.com/simen/grbl).
      smoothie is free software: you can redistribute it and/or modify it under the terms of the gnu general public license as published by the free software foundation, either version 3 of the license, or (at your option) any later version.
      smoothie is distributed in the hope that it will be useful, but without any warranty; without even the implied warranty of merchantability or fitness for a particular purpose. see the gnu general public license for more details.
      you should have received a copy of the gnu general public license along with smoothie. if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef ANGLESENSOR_H
#define ANGLESENSOR_H

#include "TempSensor.h"
#include "RingBuffer.h"
#include "Pin.h"

#include <tuple>

#define QUEUE_LEN 32

class StreamOutput;

class AngleSensor
{
    public:
        AngleSensor(uint16_t config_identifier);
        ~AngleSensor();

        // TempSensor interface.
        int get_raw_value();
//        void get_raw();

    private:
        int new_thermistor_reading();

        Pin  angle_sensor_pin;

//        struct {
//            bool bad_config:1;
//            bool use_steinhart_hart:1;
//        };
//        uint8_t sensor_number;
};

#endif
