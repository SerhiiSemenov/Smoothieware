/*
    This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
    Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
    Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
    You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Extruder.h"

#include "libs/Module.h"
#include "libs/Kernel.h"

#include "modules/robot/Conveyor.h"
#include "modules/robot/Block.h"
#include "StepperMotor.h"
#include "SlowTicker.h"
#include "Stepper.h"
#include "StepTicker.h"
#include "Config.h"
#include "StepperMotor.h"
#include "Robot.h"
#include "checksumm.h"
#include "ConfigValue.h"
#include "Gcode.h"
#include "libs/StreamOutput.h"
#include "PublicDataRequest.h"
#include "StreamOutputPool.h"
#include "ExtruderPublicAccess.h"

#include <mri.h>
#include <vector>
#include <algorithm>

// OLD config names for backwards compatibility, NOTE new configs will not be added here
#define extruder_module_enable_checksum      CHECKSUM("extruder_module_enable")
#define extruder_steps_per_mm_checksum       CHECKSUM("extruder_steps_per_mm")
#define extruder_steps_per_degree_checksum   CHECKSUM("extruder_steps_per_angle")
#define extruder_filament_diameter_checksum  CHECKSUM("extruder_filament_diameter")
#define extruder_acceleration_checksum       CHECKSUM("extruder_acceleration")
#define extruder_step_pin_checksum           CHECKSUM("extruder_step_pin")
#define extruder_dir_pin_checksum            CHECKSUM("extruder_dir_pin")
#define extruder_en_pin_checksum             CHECKSUM("extruder_en_pin")
#define extruder_max_speed_checksum          CHECKSUM("extruder_max_speed")
#define extruder_default_feed_rate_checksum  CHECKSUM("extruder_default_feed_rate")

// NEW config names

#define default_feed_rate_checksum           CHECKSUM("default_feed_rate")
#define steps_per_mm_checksum                CHECKSUM("steps_per_mm")
#define steps_per_angle_checksum             CHECKSUM("steps_per_angle")
#define filament_diameter_checksum           CHECKSUM("filament_diameter")
#define acceleration_checksum                CHECKSUM("acceleration")
#define step_pin_checksum                    CHECKSUM("step_pin")
#define dir_pin_checksum                     CHECKSUM("dir_pin")
#define en_pin_checksum                      CHECKSUM("en_pin")
#define max_speed_checksum                   CHECKSUM("max_speed")
#define x_offset_checksum                    CHECKSUM("x_offset")
#define y_offset_checksum                    CHECKSUM("y_offset")
#define z_offset_checksum                    CHECKSUM("z_offset")

#define ultrasonic_enable_pin                CHECKSUM("ultrasonic_enable_pin")
#define ultrasonic_ready_pin                 CHECKSUM("ultrasonic_ready_pin")
#define ultrasonic_status_pin                CHECKSUM("ultrasonic_status_pin")
#define ultrasonic_fault_pin                 CHECKSUM("ultrasonic_fault_pin")

#define retract_length_checksum              CHECKSUM("retract_length")
#define retract_feedrate_checksum            CHECKSUM("retract_feedrate")
#define retract_recover_length_checksum      CHECKSUM("retract_recover_length")
#define retract_recover_feedrate_checksum    CHECKSUM("retract_recover_feedrate")
#define retract_zlift_length_checksum        CHECKSUM("retract_zlift_length")
#define retract_zlift_feedrate_checksum      CHECKSUM("retract_zlift_feedrate")

#define X_AXIS      0
#define Y_AXIS      1
#define Z_AXIS      2

#define OFF 0
#define SOLO 1
#define FOLLOW 2

#define PI 3.14159265358979F


/* The extruder module controls a filament extruder for 3D printing: http://en.wikipedia.org/wiki/Fused_deposition_modeling
* It can work in two modes : either the head does not move, and the extruder moves the filament at a specified speed ( SOLO mode here )
* or the head moves, and the extruder moves plastic at a speed proportional to the movement of the head ( FOLLOW mode here ).
*/

Extruder::Extruder( uint16_t config_identifier, bool single )
{
    this->absolute_mode = true;
    this->milestone_absolute_mode = true;
    this->enabled = false;
    this->single_config = single;
    this->identifier = config_identifier;
    this->retracted = false;
    this->volumetric_multiplier = 1.0F;
    this->extruder_multiplier = 1.0F;
    this->stepper_motor = nullptr;
    this->milestone_last_position = 0;
    this->max_volumetric_rate = 0;
    this->sensor = new AngleSensor(config_identifier);
    this->current_position = 0.0F;
    this->current_angle = 0.0F;
    this->previous_angle = 0.0F;
    this->previous_angle_is_positiv = true;
    this->travel_ratio = 0.0F;
    this->travel_distance = 0.0F;
    this->travel_angle = 0.0F;
    this->saved_current_position = 0.0F;
    this->saved_current_angle = 0.0F;
    this->current_position_optimize = 0.0F;

    memset(this->offset, 0, sizeof(this->offset));
}

Extruder::~Extruder()
{
    delete stepper_motor;
}

void Extruder::on_halt(void *arg)
{
    if(arg == nullptr) {
        // turn off motor
        this->en_pin.set(1);
    }
}

void Extruder::on_module_loaded()
{
    // Settings
    this->on_config_reload(this);

    // Start values
    this->target_position = 0;
    this->current_position = 0;
    this->target_angle = 0;
    this->current_angle = 0;
    this->unstepped_distance = 0;
    this->current_block = NULL;
    this->mode = OFF;

    // We work on the same Block as Stepper, so we need to know when it gets a new one and drops one
    this->register_for_event(ON_BLOCK_BEGIN);
    this->register_for_event(ON_BLOCK_END);
    this->register_for_event(ON_GCODE_RECEIVED);
    this->register_for_event(ON_GCODE_EXECUTE);
    this->register_for_event(ON_HALT);
    this->register_for_event(ON_SPEED_CHANGE);
    this->register_for_event(ON_GET_PUBLIC_DATA);
    this->register_for_event(ON_SET_PUBLIC_DATA);

    // Update speed every *acceleration_ticks_per_second*
    THEKERNEL->step_ticker->register_acceleration_tick_handler([this]() {
        acceleration_tick();
    });
}

// Get config
void Extruder::on_config_reload(void *argument)
{
    if( this->single_config ) {
        // If this module uses the old "single extruder" configuration style

        this->steps_per_millimeter        = THEKERNEL->config->value(extruder_steps_per_mm_checksum      )->by_default(1)->as_number();
        this->steps_per_angle             = THEKERNEL->config->value(extruder_steps_per_degree_checksum  )->by_default(1)->as_number();
        this->filament_diameter           = THEKERNEL->config->value(extruder_filament_diameter_checksum )->by_default(0)->as_number();
        this->acceleration                = THEKERNEL->config->value(extruder_acceleration_checksum      )->by_default(1000)->as_number();
        this->feed_rate                   = THEKERNEL->config->value(extruder_default_feed_rate_checksum )->by_default(1000)->as_number();

        this->step_pin.from_string(         THEKERNEL->config->value(extruder_step_pin_checksum          )->by_default("nc" )->as_string())->as_output();
        this->dir_pin.from_string(          THEKERNEL->config->value(extruder_dir_pin_checksum           )->by_default("nc" )->as_string())->as_output();
        this->en_pin.from_string(           THEKERNEL->config->value(extruder_en_pin_checksum            )->by_default("nc" )->as_string())->as_output();

        for(int i = 0; i < 3; i++) {
            this->offset[i] = 0;
        }

        this->enabled = true;

    } else {
        // If this module was created with the new multi extruder configuration style

        this->steps_per_millimeter = THEKERNEL->config->value(extruder_checksum, this->identifier, steps_per_mm_checksum      )->by_default(1)->as_number();
        this->steps_per_angle      = THEKERNEL->config->value(extruder_checksum, this->identifier, steps_per_angle_checksum   )->by_default(1)->as_number();
        this->filament_diameter    = THEKERNEL->config->value(extruder_checksum, this->identifier, filament_diameter_checksum )->by_default(0)->as_number();
        this->acceleration         = THEKERNEL->config->value(extruder_checksum, this->identifier, acceleration_checksum      )->by_default(1000)->as_number();
        this->feed_rate            = THEKERNEL->config->value(extruder_checksum, this->identifier, default_feed_rate_checksum )->by_default(1000)->as_number();

        this->step_pin.from_string( THEKERNEL->config->value(extruder_checksum, this->identifier, step_pin_checksum          )->by_default("nc" )->as_string())->as_output();
        this->dir_pin.from_string(  THEKERNEL->config->value(extruder_checksum, this->identifier, dir_pin_checksum           )->by_default("nc" )->as_string())->as_output();
        this->en_pin.from_string(   THEKERNEL->config->value(extruder_checksum, this->identifier, en_pin_checksum            )->by_default("nc" )->as_string())->as_output();

        this->en_ultrasonic_pin.from_string ( THEKERNEL->config->value(extruder_checksum, this->identifier, ultrasonic_enable_pin  )->by_default("nc" )->as_string())->as_output();
        this->get_ready_pin.from_string     ( THEKERNEL->config->value(extruder_checksum, this->identifier, ultrasonic_ready_pin   )->by_default("nc" )->as_string())->as_input();
        this->get_status_pin.from_string    ( THEKERNEL->config->value(extruder_checksum, this->identifier, ultrasonic_status_pin  )->by_default("nc" )->as_string())->as_input();
        this->get_fault_pin.from_string     ( THEKERNEL->config->value(extruder_checksum, this->identifier, ultrasonic_fault_pin   )->by_default("nc" )->as_string())->as_input();

        this->offset[X_AXIS] = THEKERNEL->config->value(extruder_checksum, this->identifier, x_offset_checksum          )->by_default(0)->as_number();
        this->offset[Y_AXIS] = THEKERNEL->config->value(extruder_checksum, this->identifier, y_offset_checksum          )->by_default(0)->as_number();
        this->offset[Z_AXIS] = THEKERNEL->config->value(extruder_checksum, this->identifier, z_offset_checksum          )->by_default(0)->as_number();

    }

    // these are only supported in the new syntax, no need to be backward compatible as they did not exist before the change
    this->retract_length           = THEKERNEL->config->value(extruder_checksum, this->identifier, retract_length_checksum)->by_default(3)->as_number();
    this->retract_feedrate         = THEKERNEL->config->value(extruder_checksum, this->identifier, retract_feedrate_checksum)->by_default(45)->as_number();
    this->retract_recover_length   = THEKERNEL->config->value(extruder_checksum, this->identifier, retract_recover_length_checksum)->by_default(0)->as_number();
    this->retract_recover_feedrate = THEKERNEL->config->value(extruder_checksum, this->identifier, retract_recover_feedrate_checksum)->by_default(8)->as_number();
    this->retract_zlift_length     = THEKERNEL->config->value(extruder_checksum, this->identifier, retract_zlift_length_checksum)->by_default(0)->as_number();
    this->retract_zlift_feedrate   = THEKERNEL->config->value(extruder_checksum, this->identifier, retract_zlift_feedrate_checksum)->by_default(100 * 60)->as_number(); // mm/min

    if(filament_diameter > 0.01F) {
        this->volumetric_multiplier = 1.0F / (powf(this->filament_diameter / 2, 2) * PI);
    }

    // Stepper motor object for the extruder
    this->stepper_motor = new StepperMotor(step_pin, dir_pin, en_pin);
    this->stepper_motor->attach(this, &Extruder::stepper_motor_finished_move );
    if( this->single_config ) {
        this->stepper_motor->set_max_rate(THEKERNEL->config->value(extruder_max_speed_checksum)->by_default(1000)->as_number());
    } else {
        this->stepper_motor->set_max_rate(THEKERNEL->config->value(extruder_checksum, this->identifier, max_speed_checksum)->by_default(1000)->as_number());
    }
}

void Extruder::on_get_public_data(void *argument)
{
    PublicDataRequest *pdr = static_cast<PublicDataRequest *>(argument);

    if(!pdr->starts_with(extruder_checksum)) return;

    if(this->enabled) {
        // Note this is allowing both step/mm and filament diameter to be exposed via public data
        pdr->set_data_ptr(&this->steps_per_millimeter);
        pdr->set_taken();
    }
}

float Extruder::distance_to_angle(float dist)
{
    return (dist * this->steps_per_millimeter) / this->steps_per_angle;
}

float Extruder::angle_to_distance(float angle)
{
    return (angle * this->steps_per_angle) / this->steps_per_millimeter;
}

float Extruder::getNextEdgeAngle(float angle)
 {
    float resAngle = 180 + angle;
    if (resAngle >= 360)
    {
        resAngle = resAngle -360;
    }
    return resAngle;
}

float Extruder::getNegativeFromPositivAngle(float angle)
{
    return (360 - angle)*(-1);
}

float Extruder::getPositivFromNegativeAngle(float angle)
{
    return 360 - angle;
}

float Extruder::optimize_angle(float input_angle)
{
    std::vector<float> rotation_distance;
    std::vector<float>::iterator min_distance_it;
//    float resAngle = 0.0;

    if (input_angle < 0)
    {
       input_angle = 360 + input_angle;
    }

    if (input_angle == this->previous_angle)
    {
        this->travel_distance = 0;
        return 0;
    }

    if (MAX_ANGLE_LIMIT < input_angle || input_angle < MIN_ANGLE_LIMIT)
    {
        input_angle = (input_angle/MAX_ANGLE_LIMIT)*MAX_ANGLE_LIMIT;
    }

     /*Find distance*/
    rotation_distance.push_back(abs(this->previous_angle - input_angle));                                 //FIRST_CASE  if curent_angle > input_angle, move <--, else -->
    rotation_distance.push_back(abs(360 - abs(this->previous_angle - input_angle)));                      //SECOND_CASE if curent_angle > input_angle, move -->, else <--
    rotation_distance.push_back(abs(getNextEdgeAngle(this->previous_angle) - input_angle));               //THIRD_CASE  if 180+curent_angle > input_angle, move <--, else -->
    rotation_distance.push_back(abs(360 - abs(getNextEdgeAngle(this->previous_angle) - input_angle)));    //FOURTH_CASE if 180-curent_angle > input_angle, move -->, else <--

//    THEKERNEL->streams->printf("%4.4f, %4.4f, %4.4f, %4.4f\n\r", rotation_distance[0], rotation_distance[1], rotation_distance[2], rotation_distance[3]);
    min_distance_it = std::min_element(rotation_distance.begin(), rotation_distance.end());
    int diff_case = std::distance(rotation_distance.begin(), min_distance_it);

    if (rotation_distance[diff_case] == 0.0)
    {
        this->travel_distance = 0;
        return 0;
    }

    switch(diff_case)
    {
        case FIRST_CASE:
            // set new angle positiv
//            resAngle = this->current_position_optimize + (input_angle - this->previous_angle);
            this->travel_distance = angle_to_distance(input_angle - this->previous_angle);
        break;
        case SECOND_CASE:
            // set negative angle
            if ((this->previous_angle - input_angle) < 0)
            {
//                resAngle = this->current_position_optimize - rotation_distance[diff_case];
                this->travel_distance = angle_to_distance(rotation_distance[diff_case])*(-1);
            }
            else
            {
//                resAngle = this->current_position_optimize + rotation_distance[diff_case];
                this->travel_distance = angle_to_distance(rotation_distance[diff_case]);
            }
        break;
        case THIRD_CASE:
//            resAngle = this->current_position_optimize + (input_angle - getNextEdgeAngle(this->previous_angle));
            this->travel_distance = angle_to_distance(input_angle - getNextEdgeAngle(this->previous_angle));
        break;
        case FOURTH_CASE:
            // set new negativ angle
            if ((getNextEdgeAngle(this->previous_angle) - input_angle) < 0)
            {
//                resAngle = this->current_position_optimize + rotation_distance[diff_case];
                this->travel_distance = angle_to_distance(rotation_distance[diff_case])*(-1);
            }
            else
            {
//                resAngle = this->current_position_optimize - rotation_distance[diff_case];
                this->travel_distance = angle_to_distance(rotation_distance[diff_case]);
            }
        break;
    };

//    if (resAngle > 360)
//    {
//        resAngle = resAngle - 360;
//    }

//    THEKERNEL->streams->printf("case %d, current_a=%4.4f, input_a=%4.4f, res=%4.4f travel_distance=%4.4f\n\r", diff_case + 1, this->current_position_optimize, input_angle, resAngle, distance_to_angle(this->travel_distance));
    this->previous_angle = input_angle;
//    this->current_position_optimize = resAngle;

    return 0;
}

// check against maximum speeds and return the rate modifier
float Extruder::check_max_speeds(float target, float isecs)
{
    float rm = 1.0F; // default no rate modification
    float delta;
    // get change in E (may be mm or mm³)
    if(milestone_absolute_mode) {
        delta = fabsf(target - milestone_last_position); // delta move
        milestone_last_position = target;

    } else {
        delta = target;
        milestone_last_position += target;
    }

    if(this->max_volumetric_rate > 0 && this->filament_diameter > 0.01F) {
        // volumetric enabled and check for volumetric rate
        float v = delta * isecs; // the flow rate in mm³/sec

        // return the rate change needed to stay within the max rate
        if(v > max_volumetric_rate) {
            rm = max_volumetric_rate / v;
            isecs *= rm; // this slows the rate down for the next test
        }
        //THEKERNEL->streams->printf("requested flow rate: %f mm³/sec, corrected flow rate: %f  mm³/sec\n", v, v * rm);
    }

    // check for max speed as well
    float max_speed = this->stepper_motor->get_max_rate();
    if(max_speed > 0) {
        if(this->filament_diameter > 0.01F) {
            // volumetric so need to convert delta which is mm³ to mm
            delta *= volumetric_multiplier;
        }

        float sm = 1.0F;
        float v = delta * isecs; // the speed in mm/sec
        if(v > max_speed) {
            sm *= (max_speed / v);
        }
        //THEKERNEL->streams->printf("requested speed: %f mm/sec, corrected speed: %f  mm/sec\n", v, v * sm);
        rm *= sm;
    }
    return rm;
}

void Extruder::on_set_public_data(void *argument)
{
    PublicDataRequest *pdr = static_cast<PublicDataRequest *>(argument);

    if(!pdr->starts_with(extruder_checksum)) return;

    // handle extrude rates request from robot
    if(pdr->second_element_is(target_checksum)) {
        // disabled extruders do not reply NOTE only one enabled extruder supported
        if(!this->enabled) return;

        float *d = static_cast<float *>(pdr->get_data_ptr());
        float target = d[0]; // the E passed in on Gcode is in mm³ (maybe absolute or relative)
        float isecs = d[1]; // inverted secs

        // check against maximum speeds and return rate modifier
        d[1] = check_max_speeds(target, isecs);

        pdr->set_taken();
        return;
    }

    // save or restore state
    if(pdr->second_element_is(save_state_checksum)) {
        this->saved_current_position = this->current_position;
        this->saved_absolute_mode = this->absolute_mode;
        pdr->set_taken();
    } else if(pdr->second_element_is(restore_state_checksum)) {
        // NOTE this only gets called when the queue is empty so the milestones will be the same
        this->milestone_last_position= this->current_position = this->saved_current_position;
        this->milestone_absolute_mode= this->absolute_mode = this->saved_absolute_mode;
        pdr->set_taken();
    }
}

void Extruder::on_gcode_received(void *argument)
{
    Gcode *gcode = static_cast<Gcode *>(argument);

    // M codes most execute immediately, most only execute if enabled
    if (gcode->has_m) {
        if (gcode->m == 114 && gcode->subcode == 0 && this->enabled) {
            char buf[16];
            int n = snprintf(buf, sizeof(buf), " E:%1.3f ", this->current_position);
            gcode->txt_after_ok.append(buf, n);

        } else if (gcode->m == 92 && ( (this->enabled && !gcode->has_letter('P')) || (gcode->has_letter('P') && gcode->get_value('P') == this->identifier) ) ) {
            float spm = this->steps_per_millimeter;
            if (gcode->has_letter('E')) {
                spm = gcode->get_value('E');
                this->steps_per_millimeter = spm;
            }

            gcode->stream->printf("E:%g ", spm);
            gcode->add_nl = true;

        } else if (gcode->m == 200 && ( (this->enabled && !gcode->has_letter('P')) || (gcode->has_letter('P') && gcode->get_value('P') == this->identifier)) ) {
            if (gcode->has_letter('D')) {
                THEKERNEL->conveyor->wait_for_empty_queue(); // only apply after the queue has emptied
                this->filament_diameter = gcode->get_value('D');
                if(filament_diameter > 0.01F) {
                    this->volumetric_multiplier = 1.0F / (powf(this->filament_diameter / 2, 2) * PI);
                } else {
                    this->volumetric_multiplier = 1.0F;
                }
            } else {
                if(filament_diameter > 0.01F) {
                    gcode->stream->printf("Filament Diameter: %f\n", this->filament_diameter);
                } else {
                    gcode->stream->printf("Volumetric extrusion is disabled\n");
                }
            }

        } else if (gcode->m == 203 && ( (this->enabled && !gcode->has_letter('P')) || (gcode->has_letter('P') && gcode->get_value('P') == this->identifier)) ) {
            // M203 Exxx Vyyy Set maximum feedrates xxx mm/sec and/or yyy mm³/sec
            if(gcode->get_num_args() == 0) {
                gcode->stream->printf("E:%g V:%g", this->stepper_motor->get_max_rate(), this->max_volumetric_rate);
                gcode->add_nl = true;

            } else {
                if(gcode->has_letter('E')) {
                    this->stepper_motor->set_max_rate(gcode->get_value('E'));
                }
                if(gcode->has_letter('V')) {
                    this->max_volumetric_rate = gcode->get_value('V');
                }
            }

        } else if (gcode->m == 204 && gcode->has_letter('E') &&
                   ( (this->enabled && !gcode->has_letter('P')) || (gcode->has_letter('P') && gcode->get_value('P') == this->identifier)) ) {
            // extruder acceleration M204 Ennn mm/sec^2 (Pnnn sets the specific extruder for M500)
            this->acceleration = gcode->get_value('E');

        } else if (gcode->m == 207 && ( (this->enabled && !gcode->has_letter('P')) || (gcode->has_letter('P') && gcode->get_value('P') == this->identifier)) ) {
            // M207 - set retract length S[positive mm] F[feedrate mm/min] Z[additional zlift/hop] Q[zlift feedrate mm/min]
            if(gcode->has_letter('S')) retract_length = gcode->get_value('S');
            if(gcode->has_letter('F')) retract_feedrate = gcode->get_value('F') / 60.0F; // specified in mm/min converted to mm/sec
            if(gcode->has_letter('Z')) retract_zlift_length = gcode->get_value('Z');
            if(gcode->has_letter('Q')) retract_zlift_feedrate = gcode->get_value('Q');

        } else if (gcode->m == 208 && ( (this->enabled && !gcode->has_letter('P')) || (gcode->has_letter('P') && gcode->get_value('P') == this->identifier)) ) {
            // M208 - set retract recover length S[positive mm surplus to the M207 S*] F[feedrate mm/min]
            if(gcode->has_letter('S')) retract_recover_length = gcode->get_value('S');
            if(gcode->has_letter('F')) retract_recover_feedrate = gcode->get_value('F') / 60.0F; // specified in mm/min converted to mm/sec

        } else if (gcode->m == 221 && this->enabled) { // M221 S100 change flow rate by percentage
            if(gcode->has_letter('S')) {
                this->extruder_multiplier = gcode->get_value('S') / 100.0F;
            } else {
                gcode->stream->printf("Flow rate at %6.2f %%\n", this->extruder_multiplier * 100.0F);
            }

        } else if (gcode->m == 500 || gcode->m == 503) { // M500 saves some volatile settings to config override file, M503 just prints the settings
            if( this->single_config ) {
                gcode->stream->printf(";E Steps per mm:\nM92 E%1.4f E Steps per angle:\nM92 E%1.4f\n\r", this->steps_per_millimeter, this->steps_per_angle);
                gcode->stream->printf(";E Filament diameter:\nM200 D%1.4f\n\r", this->filament_diameter);
                gcode->stream->printf(";E retract length, feedrate, zlift length, feedrate:\nM207 S%1.4f F%1.4f Z%1.4f Q%1.4f\n\r", this->retract_length, this->retract_feedrate * 60.0F, this->retract_zlift_length, this->retract_zlift_feedrate);
                gcode->stream->printf(";E retract recover length, feedrate:\n\rM208 S%1.4f F%1.4f\n\r", this->retract_recover_length, this->retract_recover_feedrate * 60.0F);
                gcode->stream->printf(";E acceleration mm/sec²:\nM204 E%1.4f\n\r", this->acceleration);
                gcode->stream->printf(";E max feed rate mm/sec:\nM203 E%1.4f\n\r", this->stepper_motor->get_max_rate());
                if(this->max_volumetric_rate > 0) {
                    gcode->stream->printf(";E max volumetric rate mm³/sec:\nM203 V%1.4f\n\r", this->max_volumetric_rate);
                }

            } else {
                gcode->stream->printf(";E Steps per mm:\nM92 E%1.4f P%d Steps per angle: E%1.4f\n\r", this->steps_per_millimeter, this->identifier, this->steps_per_angle);
                gcode->stream->printf(";E Filament diameter:\nM200 D%1.4f P%d\n", this->filament_diameter, this->identifier);
                gcode->stream->printf(";E retract length, feedrate:\nM207 S%1.4f F%1.4f Z%1.4f Q%1.4f P%d\n", this->retract_length, this->retract_feedrate * 60.0F, this->retract_zlift_length, this->retract_zlift_feedrate, this->identifier);
                gcode->stream->printf(";E retract recover length, feedrate:\nM208 S%1.4f F%1.4f P%d\n", this->retract_recover_length, this->retract_recover_feedrate * 60.0F, this->identifier);
                gcode->stream->printf(";E acceleration mm/sec²:\nM204 E%1.4f P%d\n", this->acceleration, this->identifier);
                gcode->stream->printf(";E max feed rate mm/sec:\nM203 E%1.4f P%d\n", this->stepper_motor->get_max_rate(), this->identifier);
                if(this->max_volumetric_rate > 0) {
                    gcode->stream->printf(";E max volumetric rate mm³/sec:\nM203 V%1.4f P%d\n", this->max_volumetric_rate, this->identifier);
                }
            }

        } else if( gcode->m == 17 || gcode->m == 18 || gcode->m == 82 || gcode->m == 83 || gcode->m == 84 ) {
            // Mcodes to pass along to on_gcode_execute
            THEKERNEL->conveyor->append_gcode(gcode);

        }

    } else if(gcode->has_g) {
        // G codes, NOTE some are ignored if not enabled
        if( (gcode->g == 92 && gcode->has_letter('E')) || (gcode->g == 90 || gcode->g == 91) ) {
            // Gcodes to pass along to on_gcode_execute
            THEKERNEL->conveyor->append_gcode(gcode);

        } else if( this->enabled && gcode->g < 4 && gcode->has_letter('E') && fabsf(gcode->millimeters_of_travel) < 0.00001F ) { // With floating numbers, we can have 0 != 0, NOTE needs to be same as in Robot.cpp#745
            // NOTE was ... gcode->has_letter('E') && !gcode->has_letter('X') && !gcode->has_letter('Y') && !gcode->has_letter('Z') ) {
            // This is a SOLO move, we add an empty block to the queue to prevent subsequent gcodes being executed at the same time
            THEKERNEL->conveyor->append_gcode(gcode);
            THEKERNEL->conveyor->queue_head_block();

        } else if( this->enabled && (gcode->g == 10 || gcode->g == 11) && !gcode->has_letter('L') ) {
            // firmware retract command (Ignore if has L parameter that is not for us)
            // check we are in the correct state of retract or unretract
            if(gcode->g == 10 && !retracted) {
                this->retracted = true;
                this->cancel_zlift_restore = false;
            } else if(gcode->g == 11 && retracted) {
                this->retracted = false;
            } else
                return; // ignore duplicates

            // now we do a special hack to add zlift if needed, this should go in Robot but if it did the zlift would be executed before retract which is bad
            // this way zlift will happen after retract, (or before for unretract) NOTE we call the robot->on_gcode_receive directly to avoid recursion
            if(retract_zlift_length > 0 && gcode->g == 11 && !this->cancel_zlift_restore) {
                // reverse zlift happens before unretract
                // NOTE we do not do this if cancel_zlift_restore is set to true, which happens if there is an absolute Z move inbetween G10 and G11
                char buf[32];
                int n = snprintf(buf, sizeof(buf), "G0 Z%1.4f F%1.4f", -retract_zlift_length, retract_zlift_feedrate);
                string cmd(buf, n);
                Gcode gc(cmd, &(StreamOutput::NullStream));
                THEKERNEL->robot->push_state(); // save state includes feed rates etc
                THEKERNEL->robot->absolute_mode = false; // needs to be relative mode
                THEKERNEL->robot->on_gcode_received(&gc); // send to robot directly
                THEKERNEL->robot->pop_state(); // restore state includes feed rates etc
            }

            // This is a solo move, we add an empty block to the queue to prevent subsequent gcodes being executed at the same time
            THEKERNEL->conveyor->append_gcode(gcode);
            THEKERNEL->conveyor->queue_head_block();

            if(retract_zlift_length > 0 && gcode->g == 10) {
                char buf[32];
                int n = snprintf(buf, sizeof(buf), "G0 Z%1.4f F%1.4f", retract_zlift_length, retract_zlift_feedrate);
                string cmd(buf, n);
                Gcode gc(cmd, &(StreamOutput::NullStream));
                THEKERNEL->robot->push_state(); // save state includes feed rates etc
                THEKERNEL->robot->absolute_mode = false; // needs to be relative mode
                THEKERNEL->robot->on_gcode_received(&gc); // send to robot directly
                THEKERNEL->robot->pop_state(); // restore state includes feed rates etc
            }

        } else if( this->enabled && this->retracted && (gcode->g == 0 || gcode->g == 1) && gcode->has_letter('Z')) {
            // NOTE we cancel the zlift restore for the following G11 as we have moved to an absolute Z which we need to stay at
            this->cancel_zlift_restore = true;
        }
    }

    // handle some codes now for the volumetric rate limiting
    // G28 G90 G91 G92 M82 M83
    if(gcode->has_m) {
        switch(gcode->m) {
            case 50:        //Enable ultrasonic generator
                this->en_ultrasonic_pin.set(true);
                break;
            case 51:
                this->en_ultrasonic_pin.set(false);
                break;
            case 52:
                THEKERNEL->streams->printf("%s\n\r", this->get_ready_pin.get() ? "true" : "false");
                break;
            case 53:
                THEKERNEL->streams->printf("%s\n\r", this->get_status_pin.get() ? "true" : "false");
                break;
            case 54:
                THEKERNEL->streams->printf("%s\n\r", this->get_fault_pin.get() ? "true" : "false");
                break;
            case 82: this->milestone_absolute_mode = true; break;
            case 83: this->milestone_absolute_mode = false; break;
        }

    } else if(gcode->has_g) {
        switch(gcode->g) {
            case 90: this->milestone_absolute_mode = true; break;
            case 91: this->milestone_absolute_mode = false; break;
            case 92:
                if(this->enabled) {
                    if(gcode->has_letter('E')) {
                        this->milestone_last_position = gcode->get_value('E');
                    } else if(gcode->get_num_args() == 0) {
                        this->milestone_last_position = 0;
                    }
                }
                break;
            case 28:
                this->current_angle = 0;
                if (gcode->has_letter('E'))
                {
                    this->do_home();
                }
                //TODO: remove
                this->target_position = 0;
                this->current_position = 0;
                this->target_angle = 0;
                this->current_angle = 0;
                this->unstepped_distance = 0;
                this->travel_ratio = 0.0F;
                this->travel_distance = 0.0F;
                this->travel_angle = 0.0F;
                this->saved_current_position = 0.0F;
                this->saved_current_angle = 0.0F;
                this->current_position_optimize = 0.0F;
                break;
        }
    }
}

// Compute extrusion speed based on parameters and gcode distance of travel
void Extruder::on_gcode_execute(void *argument)
{
    Gcode *gcode = static_cast<Gcode *>(argument);

    // The mode is OFF by default, and SOLO or FOLLOW only if we need to extrude
    this->mode = OFF;

    // Absolute/relative mode, globably modal affect all extruders whether enabled or not
    if( gcode->has_m ) {
        switch(gcode->m) {
            case 17:
                this->en_pin.set(0);
                break;
            case 18:
                this->en_pin.set(1);
                break;
            case 82:
                this->absolute_mode = true;
                break;
            case 83:
                this->absolute_mode = false;
                break;
            case 84:
                this->en_pin.set(1);
                break;
        }
        return;

    } else if( gcode->has_g && (gcode->g == 90 || gcode->g == 91) ) {
        this->absolute_mode = (gcode->g == 90);
        return;
    }


    if( gcode->has_g && this->enabled ) {
        // G92: Reset extruder position
        if( gcode->g == 92 ) {
            if( gcode->has_letter('E') ) {
                this->current_angle = optimize_angle(gcode->get_value('E'));
                this->target_angle  = this->current_angle;
                this->current_position = angle_to_distance(this->current_angle);
                this->target_position  = this->current_position;
                this->unstepped_distance = 0;
            } else if( gcode->get_num_args() == 0) {
                this->current_position = 0.0;
                this->target_position = this->current_position;
                this->current_angle = 0.0;
                this->target_angle = this->current_angle;
                this->unstepped_distance = 0;
            }

        } else if (gcode->g <= 3) {
            // Extrusion length from 'G' Gcode
            if( gcode->has_letter('E' )) {
                // Get relative extrusion distance depending on mode ( in absolute mode we must subtract target_position )
                float angle = optimize_angle(gcode->get_value('E'));
                float relative_angle = angle;
                float extrusion_distance = angle_to_distance(angle);
                float relative_extrusion_distance = extrusion_distance;
                if (this->absolute_mode) {
                    relative_extrusion_distance -= this->target_position;
                    this->target_position = extrusion_distance;
                    relative_angle -= this->target_angle;
                    this->target_angle = angle;
                } else {
                    this->target_position += relative_extrusion_distance;
                    this->target_angle += relative_angle;
                }
                // If the robot is moving, we follow it's movement, otherwise, we move alone
                if( fabsf(gcode->millimeters_of_travel) < 0.00001F ) { // With floating numbers, we can have 0 != 0, NOTE needs to be same as in Robot.cpp#745
                    this->mode = SOLO;
//                    this->travel_distance = relative_extrusion_distance;
                } else {
                    // We move proportionally to the robot's movement
                    this->mode = FOLLOW;
                    this->travel_ratio = (relative_extrusion_distance * this->volumetric_multiplier * this->extruder_multiplier) / gcode->millimeters_of_travel; // adjust for volumetric extrusion and extruder multiplier
                }
                this->en_pin.set(0);
            }

            // NOTE this is only used in SOLO mode, but any F on a G0/G1 will set the speed for future retracts that are not firmware retracts
            if (gcode->has_letter('F')) {
                feed_rate = gcode->get_value('F') / THEKERNEL->robot->get_seconds_per_minute();
                if (stepper_motor->get_max_rate() > 0 && feed_rate > stepper_motor->get_max_rate())
                    feed_rate = stepper_motor->get_max_rate();
            }
        }
    }
}

// When a new block begins, either follow the robot, or step by ourselves ( or stay back and do nothing )
void Extruder::on_block_begin(void *argument)
{
    if(!this->enabled) return;

    if( this->mode == OFF ) {
        this->current_block = NULL;
        this->stepper_motor->set_moved_last_block(false);
        return;
    }

    Block *block = static_cast<Block *>(argument);
    if( this->mode == FOLLOW ) {
        // In FOLLOW mode, we just follow the stepper module
//        this->travel_distance = block->millimeters * this->travel_ratio;
    }

    // common for both FOLLOW and SOLO
    this->current_position += this->travel_distance ;
    this->current_angle += distance_to_angle(this->travel_distance);

    if (this->current_angle > 360)
    {
        this->current_angle = this->current_angle - 360;
        this->current_position = angle_to_distance(this->current_angle);
    }

    // round down, we take care of the fractional part next time
    int steps_to_step = abs((int)floorf(this->steps_per_millimeter * (this->travel_distance + this->unstepped_distance) ));

    // accumulate the fractional part
    if ( this->travel_distance > 0 ) {
        this->unstepped_distance += this->travel_distance - (steps_to_step / this->steps_per_millimeter);
    } else {
        this->unstepped_distance += this->travel_distance + (steps_to_step / this->steps_per_millimeter);
    }

    if( steps_to_step != 0 ) {
        // We take the block, we have to release it or everything gets stuck
        block->take();
        this->current_block = block;
        this->stepper_motor->move( (this->travel_distance > 0), steps_to_step);

        if(this->mode == FOLLOW) {
            on_speed_change(this); // set initial speed
            this->stepper_motor->set_moved_last_block(true);
        } else {
            // SOLO
            uint32_t target_rate = floorf(this->feed_rate * this->steps_per_millimeter);
            this->stepper_motor->set_speed(min( target_rate, rate_increase() ));  // start at first acceleration step
            this->stepper_motor->set_moved_last_block(false);
        }

    } else {
        // no steps to take this time
        this->current_block = NULL;
        this->stepper_motor->set_moved_last_block(false);
    }

}

// When a block ends, pause the stepping interrupt
void Extruder::on_block_end(void *argument)
{
    if(!this->enabled) return;
    this->current_block = NULL;
}

uint32_t Extruder::rate_increase() const
{
    return floorf((this->acceleration / THEKERNEL->acceleration_ticks_per_second) * this->steps_per_millimeter);
}

// Called periodically to change the speed to match acceleration or to match the speed of the robot
// Only used in SOLO mode
void Extruder::acceleration_tick(void)
{
    // Avoid trying to work when we really shouldn't ( between blocks or re-entry )
    if(!this->enabled || this->mode != SOLO || this->current_block == NULL || !this->stepper_motor->is_moving() ) {
        return;
    }

    uint32_t current_rate = this->stepper_motor->get_steps_per_second();
    uint32_t target_rate = floorf(this->feed_rate * this->steps_per_millimeter);

    if( current_rate < target_rate ) {
        current_rate = min( target_rate, current_rate + rate_increase() );
        // steps per second
        this->stepper_motor->set_speed(current_rate);
    }

    return;
}

// Speed has been updated for the robot's stepper, we must update accordingly
void Extruder::on_speed_change( void *argument )
{
    // Avoid trying to work when we really shouldn't ( between blocks or re-entry )
    if(!this->enabled || this->current_block == NULL || this->mode != FOLLOW || !this->stepper_motor->is_moving()) {
        return;
    }

    // if we are flushing the queue we need to stop the motor when it has decelerated to zero, we get this call with argumnet == 0 when this happens
    // this is what steppermotor does
    if(argument == 0) {
        this->stepper_motor->move(0, 0);
        this->current_block->release();
        this->current_block = NULL;
        return;
    }

    /*
    * nominal block duration = current block's steps / ( current block's nominal rate )
    * nominal extruder rate = extruder steps / nominal block duration
    * actual extruder rate = nominal extruder rate * ( ( stepper's steps per second ) / ( current block's nominal rate ) )
    * or actual extruder rate = ( ( extruder steps * ( current block's nominal_rate ) ) / current block's steps ) * ( ( stepper's steps per second ) / ( current block's nominal rate ) )
    * or simplified : extruder steps * ( stepper's steps per second ) ) / current block's steps
    * or even : ( stepper steps per second ) * ( extruder steps / current block's steps )
    */

    this->stepper_motor->set_speed(THEKERNEL->stepper->get_trapezoid_adjusted_rate() * (float)this->stepper_motor->get_steps_to_move() / (float)this->current_block->steps_event_count);
}

// When the stepper has finished it's move
uint32_t Extruder::stepper_motor_finished_move(uint32_t dummy)
{
    if(!this->enabled) return 0;

    //printf("extruder releasing\r\n");

    if (this->current_block) { // this should always be true, but sometimes it isn't. TODO: find out why
        Block *block = this->current_block;
        this->current_block = NULL;
        block->release();
    }
    return 0;

}

void Extruder::do_home(void)
{
    float current_search_angle = 0;
    float angle;
    std::vector<int> sensor_value;
    std::vector<int>::iterator max_element_it;

    //Prepare motor
    this->stepper_motor->enable(true);
    this->stepper_motor->set_speed(10000);
    this->stepper_motor->set_moved_last_block(true);

    //Find zero angle
    while(current_search_angle <= DEGREE_OF_CYCLE)
    {
        this->stepper_motor->move(true, RAW_SEARCH_ANGLE * this->steps_per_angle, 1000);
        current_search_angle += RAW_SEARCH_ANGLE;
        sensor_value.push_back(this->sensor->get_raw_value());
        while(this->stepper_motor->is_moving())
        {
            if(THEKERNEL->is_halted())
            {
                THEKERNEL->streams->printf("Operation halted \n\r");
                return;
            }
        }
    }
    max_element_it = std::max_element(sensor_value.begin(), sensor_value.end());
    angle = (std::distance(sensor_value.begin(), max_element_it)) * RAW_SEARCH_ANGLE;
    //Set zero angle
    this->stepper_motor->move(true, angle * this->steps_per_angle, 1000);
    while(this->stepper_motor->is_moving())
    {
        if(THEKERNEL->is_halted())
        {
            THEKERNEL->streams->printf("Operation halted \n\r");
            return;
        }
    }
}
