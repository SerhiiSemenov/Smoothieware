/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "AngleSensor.h"
#include "libs/Kernel.h"
#include "libs/Pin.h"
#include "Config.h"
#include "checksumm.h"
#include "Adc.h"
#include "ConfigValue.h"
#include "libs/Median.h"
#include "utils.h"
#include "StreamOutputPool.h"

#include "MRI_Hooks.h"

#define angle_sensor_checksum                   CHECKSUM("angle_sensor")
#define angle_sensor_pin_checksum               CHECKSUM("angle_sensor_pin")

AngleSensor::AngleSensor(uint16_t config_identifier)
{
    std::string pin_conf_string = THEKERNEL->config->value(angle_sensor_checksum, config_identifier, angle_sensor_pin_checksum )->required()->as_string();
    THEKERNEL->streams->printf(pin_conf_string.c_str());
    this->angle_sensor_pin.from_string(pin_conf_string);
    THEKERNEL->adc->enable_pin(&angle_sensor_pin);
}

AngleSensor::~AngleSensor()
{
}

int AngleSensor::get_raw_value()
{
    return THEKERNEL->adc->read(&angle_sensor_pin);;
}

