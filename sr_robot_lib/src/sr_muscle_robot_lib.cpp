/**
 * @file   sr_muscle_robot_lib.cpp
 * @author Ugo Cupcic <ugo@shadowrobot.com>, Toni Oliver <toni@shadowrobot.com>, contact <software@shadowrobot.com>
 * @date   Tue Mar  19 17:12:13 2013
 *
 *
 * Copyright 2013 Shadow Robot Company Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * @brief This is a generic robot library for Shadow Robot's muscle-actuated Hardware.
 *
 *
 */

#include "sr_robot_lib/sr_muscle_robot_lib.hpp"
#include <string>
#include <boost/foreach.hpp>

#include <sys/time.h>

#include <ros/ros.h>

#include <pr2_mechanism_msgs/ListControllers.h>

#define SERIOUS_ERROR_FLAGS PALM_0300_EDC_SERIOUS_ERROR_FLAGS
#define error_flag_names palm_0300_edc_error_flag_names

#define MUSCLE_PRESSURES_PER_PACKET     5

namespace shadow_robot
{

  template <class StatusType, class CommandType>
  SrMuscleRobotLib<StatusType, CommandType>::SrMuscleRobotLib(pr2_hardware_interface::HardwareInterface *hw)
    : SrRobotLib<StatusType, CommandType>(hw),
      muscle_current_state(operation_mode::device_update_state::INITIALIZATION)
  {
#ifdef DEBUG_PUBLISHER
    debug_motor_indexes_and_data.resize(nb_debug_publishers_const);
    for( int i = 0; i < nb_debug_publishers_const; ++i )
    {
      std::stringstream ss;
      ss << "srh/debug_" << i;
      debug_publishers.push_back(node_handle.advertise<std_msgs::Int16>(ss.str().c_str(),100));
    }
#endif

  }

  template <class StatusType, class CommandType>
  void SrMuscleRobotLib<StatusType, CommandType>::update(StatusType* status_data)
  {
    //read the PIC idle time
    this->main_pic_idle_time = status_data->idle_time_us;
    if (status_data->idle_time_us < this->main_pic_idle_time_min)
      this->main_pic_idle_time_min = status_data->idle_time_us;

    //get the current timestamp
    struct timeval tv;
    double timestamp = 0.0;
    if (gettimeofday(&tv, NULL))
    {
      ROS_WARN("SrMuscleRobotLib: Failed to get system time, timestamp in state will be zero");
    }
    else
    {
      timestamp = double(tv.tv_sec) + double(tv.tv_usec) / 1.0e+6;
    }

    //First we read the tactile sensors information
    this->update_tactile_info(status_data);

    //then we read the joints informations
    boost::ptr_vector<shadow_joints::Joint>::iterator joint_tmp = this->joints_vector.begin();
    for (; joint_tmp != this->joints_vector.end(); ++joint_tmp)
    {
      sr_actuator::SrActuatorState* actuator_state = this->get_joint_actuator_state(joint_tmp);

      boost::shared_ptr<shadow_joints::MuscleWrapper> muscle_wrapper = boost::static_pointer_cast<shadow_joints::MuscleWrapper>(joint_tmp->actuator_wrapper);

      actuator_state->is_enabled_ = 1;
      //actuator_state->device_id_ = muscle_wrapper->muscle_id[0];
      actuator_state->halted_ = false;

      //Fill in the tactiles.
      if( this->tactiles != NULL )
        actuator_state->tactiles_ = this->tactiles->get_tactile_data();

      this->process_position_sensor_data(joint_tmp, status_data, timestamp);

      //if no muscle is associated to this joint, then continue
      if ((muscle_wrapper->muscle_driver_id[0] == -1))
        continue;

      read_additional_data(joint_tmp, status_data);
    } //end for joint
  } //end update()

  template <class StatusType, class CommandType>
  void SrMuscleRobotLib<StatusType, CommandType>::build_command(CommandType* command)
  {
    if (muscle_current_state == operation_mode::device_update_state::INITIALIZATION)
    {
      muscle_current_state = motor_updater_->build_init_command(command);
    }
    else
    {
      //build the motor command
      muscle_current_state = motor_updater_->build_command(command);
    }

    //Build the tactile sensors command
    this->build_tactile_command(command);

    ///////
    // Now we chose the command to send to the motor
    // by default we send a torque demand (we're running
    // the force control on the motors), but if we have a waiting
    // configuration, a reset command, or a motor system control
    // request then we send the configuration
    // or the reset.
    if ( reset_motors_queue.empty()
         && motor_system_control_flags_.empty() )
    {
      command->to_motor_data_type = MOTOR_DEMAND_TORQUE;

      //loop on all the joints and update their motor: we're sending commands to all the motors.
      boost::ptr_vector<shadow_joints::Joint>::iterator joint_tmp = this->joints_vector.begin();
      for (; joint_tmp != this->joints_vector.end(); ++joint_tmp)
      {
        boost::shared_ptr<shadow_joints::MotorWrapper> motor_wrapper = boost::static_pointer_cast<shadow_joints::MotorWrapper>(joint_tmp->actuator_wrapper);

        if (joint_tmp->has_actuator)
        {
          if( !this->nullify_demand_ )
          {
            //We send the computed demand
            command->motor_data[motor_wrapper->motor_id] = motor_wrapper->actuator->command_.effort_;
          }
          else
          {
            //We want to send a demand of 0
            command->motor_data[motor_wrapper->motor_id] = 0;
          }

#ifdef DEBUG_PUBLISHER
          //publish the debug values for the given motors.
          // NB: debug_motor_indexes_and_data is smaller
          //     than debug_publishers.
          int publisher_index = 0;
          boost::shared_ptr<std::pair<int,int> > debug_pair;
          if( debug_mutex.try_lock() )
          {
            BOOST_FOREACH(debug_pair, debug_motor_indexes_and_data)
            {
              if( debug_pair != NULL )
              {
                //check if we want to publish some data for the current motor
                if( debug_pair->first == joint_tmp->actuator_wrapper->motor_id )
                {
                  //check if it's the correct data
                  if( debug_pair->second == -1 )
                  {
                    msg_debug.data = joint_tmp->actuator_wrapper->actuator->command_.effort_;
                    debug_publishers[publisher_index].publish(msg_debug);
                  }
                }
              }
              publisher_index ++;
            }

            debug_mutex.unlock();
          } //end try_lock
#endif

          joint_tmp->actuator_wrapper->actuator->state_.last_commanded_effort_ = joint_tmp->actuator_wrapper->actuator->command_.effort_;
        } //end if has_actuator
      } // end for each joint
    } //endif
    else
    {
      if ( !motor_system_control_flags_.empty() )
      {
        //treat the first waiting system control and remove it from the queue
        std::vector<sr_robot_msgs::MotorSystemControls> system_controls_to_send;
        system_controls_to_send = motor_system_control_flags_.front();
        motor_system_control_flags_.pop();

        //set the correct type of command to send to the hand.
        command->to_motor_data_type = MOTOR_SYSTEM_CONTROLS;

        std::vector<sr_robot_msgs::MotorSystemControls>::iterator it;
        for( it = system_controls_to_send.begin(); it != system_controls_to_send.end(); ++it)
        {
          short combined_flags = 0;
          if( it->enable_backlash_compensation )
            combined_flags |= MOTOR_SYSTEM_CONTROL_BACKLASH_COMPENSATION_ENABLE;
          else
            combined_flags |= MOTOR_SYSTEM_CONTROL_BACKLASH_COMPENSATION_DISABLE;

          if( it->increase_sgl_tracking )
            combined_flags |= MOTOR_SYSTEM_CONTROL_SGL_TRACKING_INC;
          if( it->decrease_sgl_tracking )
            combined_flags |= MOTOR_SYSTEM_CONTROL_SGL_TRACKING_DEC;

          if( it->increase_sgr_tracking )
            combined_flags |= MOTOR_SYSTEM_CONTROL_SGR_TRACKING_INC;
          if( it->decrease_sgr_tracking )
            combined_flags |= MOTOR_SYSTEM_CONTROL_SGR_TRACKING_DEC;

          if( it->initiate_jiggling )
            combined_flags |= MOTOR_SYSTEM_CONTROL_INITIATE_JIGGLING;

          if( it->write_config_to_eeprom )
            combined_flags |= MOTOR_SYSTEM_CONTROL_EEPROM_WRITE;

          command->motor_data[ it->motor_id ] = combined_flags;
        }
      } //end if motor_system_control_flags_.empty
      else
      {
        if (!reset_motors_queue.empty())
        {
          //reset the CAN messages counters for the motor we're going to reset.
          short motor_id = reset_motors_queue.front();
          boost::ptr_vector<shadow_joints::Joint>::iterator joint_tmp = this->joints_vector.begin();
          for (; joint_tmp != this->joints_vector.end(); ++joint_tmp)
          {
            boost::shared_ptr<shadow_joints::MotorWrapper> motor_wrapper = boost::static_pointer_cast<shadow_joints::MotorWrapper>(joint_tmp->actuator_wrapper);
            sr_actuator::SrActuatorState* actuator_state = this->get_joint_actuator_state(joint_tmp);

            if( motor_wrapper->motor_id == motor_id )
            {
              actuator_state->can_msgs_transmitted_ = 0;
              actuator_state->can_msgs_received_ = 0;
            }
          }

          //we have some reset command waiting.
          // We'll send all of them
          command->to_motor_data_type = MOTOR_SYSTEM_RESET;

          while (!reset_motors_queue.empty())
          {
            motor_id = reset_motors_queue.front();
            reset_motors_queue.pop();

            // we send the MOTOR_RESET_SYSTEM_KEY
            // and the motor id (on the bus)
            crc_unions::union16 to_send;
            to_send.byte[1] = MOTOR_SYSTEM_RESET_KEY >> 8;
            if (motor_id > 9)
              to_send.byte[0] = motor_id - 10;
            else
              to_send.byte[0] = motor_id;

            command->motor_data[motor_id] = to_send.word;
          }
        } // end if reset queue not empty
      } // end else motor_system_control_flags_.empty
    } //endelse reset_queue.empty()
  }

  template <class StatusType, class CommandType>
  void SrMuscleRobotLib<StatusType, CommandType>::add_diagnostics(std::vector<diagnostic_msgs::DiagnosticStatus> &vec,
                                   diagnostic_updater::DiagnosticStatusWrapper &d)
  {
    boost::ptr_vector<shadow_joints::Joint>::iterator joint = this->joints_vector.begin();
    for (; joint != this->joints_vector.end(); ++joint)
    {
      std::stringstream name;
      name.str("");
      name << "SRDMotor " << joint->joint_name;
      d.name = name.str();

      boost::shared_ptr<shadow_joints::MotorWrapper> actuator_wrapper = boost::static_pointer_cast<shadow_joints::MotorWrapper>(joint->actuator_wrapper);

      if (joint->has_actuator)
      {
        const sr_actuator::SrMotorActuatorState *state(&(actuator_wrapper->actuator)->state_);

        if (actuator_wrapper->actuator_ok)
        {
          if (actuator_wrapper->bad_data)
          {
            d.summary(d.WARN, "WARNING, bad CAN data received");

            d.clear();
            d.addf("Motor ID", "%d", actuator_wrapper->motor_id);
          }
          else //the data is good
          {
            d.summary(d.OK, "OK");

            d.clear();
            d.addf("Motor ID", "%d", actuator_wrapper->motor_id);
            d.addf("Motor ID in message", "%d", actuator_wrapper->msg_motor_id);
            d.addf("Serial Number", "%d", state->serial_number );
            d.addf("Assembly date", "%d / %d / %d", state->assembly_data_day, state->assembly_data_month, state->assembly_data_year );

            d.addf("Strain Gauge Left", "%d", state->strain_gauge_left_);
            d.addf("Strain Gauge Right", "%d", state->strain_gauge_right_);

            //if some flags are set
            std::stringstream ss;
            if (state->flags_.size() > 0)
            {
              int flags_seriousness = d.OK;
              std::pair<std::string, bool> flag;
              BOOST_FOREACH(flag, state->flags_)
              {
                //Serious error flag
                if (flag.second)
                  flags_seriousness = d.ERROR;

                if (flags_seriousness != d.ERROR)
                  flags_seriousness = d.WARN;
                ss << flag.first << " | ";
              }
              d.summary(flags_seriousness, ss.str().c_str());
            }
            else
              ss << " None";
            d.addf("Motor Flags", "%s", ss.str().c_str());

            d.addf("Measured PWM", "%d", state->pwm_);
            d.addf("Measured Current", "%f", state->last_measured_current_);
            d.addf("Measured Voltage", "%f", state->motor_voltage_);
            d.addf("Measured Effort", "%f", state->last_measured_effort_);
            d.addf("Temperature", "%f", state->temperature_);

            d.addf("Unfiltered position", "%f", state->position_unfiltered_);
            d.addf("Unfiltered force", "%f", state->force_unfiltered_);

            d.addf("Gear Ratio", "%d", state->motor_gear_ratio);

            d.addf("Number of CAN messages received", "%lld", state->can_msgs_received_);
            d.addf("Number of CAN messages transmitted", "%lld", state->can_msgs_transmitted_);

            d.addf("Force control Pterm", "%d", state->force_control_pterm);
            d.addf("Force control Iterm", "%d", state->force_control_iterm);
            d.addf("Force control Dterm", "%d", state->force_control_dterm);

            d.addf("Force control F", "%d", state->force_control_f_);
            d.addf("Force control P", "%d", state->force_control_p_);
            d.addf("Force control I", "%d", state->force_control_i_);
            d.addf("Force control D", "%d", state->force_control_d_);
            d.addf("Force control Imax", "%d", state->force_control_imax_);
            d.addf("Force control Deadband", "%d", state->force_control_deadband_);
            d.addf("Force control Frequency", "%d", state->force_control_frequency_);

            if (state->force_control_sign_ == 0)
              d.addf("Force control Sign", "+");
            else
              d.addf("Force control Sign", "-");

            d.addf("Last Commanded Effort", "%f", state->last_commanded_effort_);

            d.addf("Encoder Position", "%f", state->position_);

            if (state->firmware_modified_)
              d.addf("Firmware svn revision (server / pic / modified)", "%d / %d / True",
                     state->server_firmware_svn_revision_, state->pic_firmware_svn_revision_);
            else
              d.addf("Firmware svn revision (server / pic / modified)", "%d / %d / False",
                     state->server_firmware_svn_revision_, state->pic_firmware_svn_revision_);
          }
        }
        else
        {
          d.summary(d.ERROR, "Motor error");
          d.clear();
          d.addf("Motor ID", "%d", actuator_wrapper->motor_id);
        }
      }
      else
      {
        d.summary(d.OK, "No motor associated to this joint");
        d.clear();
      }
      vec.push_back(d);

    } //end for each joints

  }


  template <class StatusType, class CommandType>
  void SrMuscleRobotLib<StatusType, CommandType>::read_additional_data(boost::ptr_vector<shadow_joints::Joint>::iterator joint_tmp, ETHERCAT_DATA_STRUCTURE_0300_PALM_EDC_STATUS* status_data)
  {
    boost::shared_ptr<shadow_joints::MuscleWrapper> muscle_wrapper = boost::static_pointer_cast<shadow_joints::MuscleWrapper>(joint_tmp->actuator_wrapper);

    //check the masks to see if the CAN messages arrivedfrom the muscle driver
    //the flag should be set to 1 for each muscle. Every actuator has two muscles, so we check both flags to decide that the actuator is OK
    muscle_wrapper->actuator_ok = sr_math_utils::is_bit_mask_index_true(status_data->which_pressure_data_arrived,
                                                                        muscle_wrapper->muscle_driver_id[0] * 10 + muscle_wrapper->muscle_id[0])
                               && sr_math_utils::is_bit_mask_index_true(status_data->which_pressure_data_arrived,
                                                                        muscle_wrapper->muscle_driver_id[1] * 10 + muscle_wrapper->muscle_id[1]);

    //check the masks to see if a bad CAN message arrived
    //the flag should be 0
    joint_tmp->actuator_wrapper->bad_data = sr_math_utils::is_bit_mask_index_true(status_data->which_pressure_data_arrived,
                                                                                  muscle_wrapper->muscle_driver_id[0] * 10 + muscle_wrapper->muscle_id[0])
                                         && sr_math_utils::is_bit_mask_index_true(status_data->which_pressure_data_had_errors,
                                                                                  muscle_wrapper->muscle_driver_id[1] * 10 + muscle_wrapper->muscle_id[1]);

    crc_unions::union16 tmp_value;

    if (muscle_wrapper->actuator_ok && !(muscle_wrapper->bad_data))
    {
      sr_actuator::SrMuscleActuator* actuator = static_cast<sr_actuator::SrMuscleActuator*>(joint_tmp->actuator_wrapper->actuator);

#ifdef DEBUG_PUBLISHER
      int publisher_index = 0;
      //publish the debug values for the given motors.
      // NB: debug_motor_indexes_and_data is smaller
      //     than debug_publishers.
      boost::shared_ptr<std::pair<int,int> > debug_pair;

      if( debug_mutex.try_lock() )
      {
        BOOST_FOREACH(debug_pair, debug_motor_indexes_and_data)
        {
          if( debug_pair != NULL )
          {
            //check if we want to publish some data for the current motor
            if( debug_pair->first == joint_tmp->actuator_wrapper->motor_id )
            {
              //if < 0, then we're not asking for a FROM_MOTOR_DATA_TYPE
              if( debug_pair->second > 0 )
              {
                //check if it's the correct data
                if( debug_pair->second == status_data->motor_data_type )
                {
                  msg_debug.data = status_data->motor_data_packet[index_motor_in_msg].misc;
                  debug_publishers[publisher_index].publish(msg_debug);
                }
              }
            }
          }
          publisher_index ++;
        }

        debug_mutex.unlock();
      } //end try_lock
#endif

      //we received the data and it was correct
      bool read_torque = true;
      switch (status_data->muscle_data_type)
      {
      case MUSCLE_DATA_PRESSURE:
        actuator->state_.pressure_[0] = static_cast<int16u>(get_muscle_pressure(muscle_wrapper->muscle_driver_id[0], muscle_wrapper->muscle_id[0], status_data));
        actuator->state_.pressure_[1] = static_cast<int16u>(get_muscle_pressure(muscle_wrapper->muscle_driver_id[1], muscle_wrapper->muscle_id[1], status_data));

#ifdef DEBUG_PUBLISHER
        if( joint_tmp->actuator_wrapper->motor_id == 8 )
        {
          //ROS_ERROR_STREAM("SGL " <<actuator->state_.strain_gauge_left_);
          msg_debug.data = actuator->state_.strain_gauge_left_;
          debug_publishers[0].publish(msg_debug);
        }
#endif
        break;

      case MOTOR_DATA_PWM:
        actuator->state_.pwm_ =
          static_cast<int>(static_cast<int16s>(status_data->motor_data_packet[index_motor_in_msg].misc));
        break;
      case MOTOR_DATA_FLAGS:
        actuator->state_.flags_ = humanize_flags(status_data->motor_data_packet[index_motor_in_msg].misc);
        break;
      case MOTOR_DATA_CURRENT:
        //we're receiving the current in milli amps
        actuator->state_.last_measured_current_ =
          static_cast<double>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc))
          / 1000.0;

#ifdef DEBUG_PUBLISHER
        if( joint_tmp->actuator_wrapper->motor_id == 8 )
        {
          //ROS_ERROR_STREAM("Current " <<actuator->state_.last_measured_current_);
          msg_debug.data = static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc);
          debug_publishers[2].publish(msg_debug);
        }
#endif
        break;
      case MOTOR_DATA_VOLTAGE:
        actuator->state_.motor_voltage_ =
          static_cast<double>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc)) / 256.0;

#ifdef DEBUG_PUBLISHER
        if( joint_tmp->actuator_wrapper->motor_id == 8 )
        {
          //ROS_ERROR_STREAM("Voltage " <<actuator->state_.motor_voltage_);
          msg_debug.data = static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc);
          debug_publishers[3].publish(msg_debug);
        }
#endif
        break;
      case MOTOR_DATA_TEMPERATURE:
        actuator->state_.temperature_ =
          static_cast<double>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc)) / 256.0;
        break;
      case MOTOR_DATA_CAN_NUM_RECEIVED:
        // those are 16 bits values and will overflow -> we compute the real value.
        // This needs to be updated faster than the overflowing period (which should be roughly every 30s)
        actuator->state_.can_msgs_received_ = sr_math_utils::counter_with_overflow(
          actuator->state_.can_msgs_received_,
          static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
        break;
      case MOTOR_DATA_CAN_NUM_TRANSMITTED:
        // those are 16 bits values and will overflow -> we compute the real value.
        // This needs to be updated faster than the overflowing period (which should be roughly every 30s)
        actuator->state_.can_msgs_transmitted_ = sr_math_utils::counter_with_overflow(
          actuator->state_.can_msgs_received_,
          static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
        break;

      case MOTOR_DATA_SLOW_MISC:
        //We received a slow data:
        // the slow data type is contained in .torque, while
        // the actual data is in .misc.
        // so we won't read torque information from .torque
        read_torque = false;

        switch (static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].torque))
        {
        case MOTOR_SLOW_DATA_SVN_REVISION:
          actuator->state_.pic_firmware_svn_revision_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_SVN_SERVER_REVISION:
          actuator->state_.server_firmware_svn_revision_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_SVN_MODIFIED:
          actuator->state_.firmware_modified_ =
            static_cast<bool>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_SERIAL_NUMBER_LOW:
          actuator->state_.set_serial_number_low (
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc)) );
          break;
        case MOTOR_SLOW_DATA_SERIAL_NUMBER_HIGH:
          actuator->state_.set_serial_number_high (
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc)) );
          break;
        case MOTOR_SLOW_DATA_GEAR_RATIO:
          actuator->state_.motor_gear_ratio =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_ASSEMBLY_DATE_YYYY:
          actuator->state_.assembly_data_year =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_ASSEMBLY_DATE_MMDD:
          actuator->state_.assembly_data_month =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc)
                                      >> 8);
          actuator->state_.assembly_data_day =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc)
                                      && 0x00FF);
          break;
        case MOTOR_SLOW_DATA_CONTROLLER_F:
          actuator->state_.force_control_f_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_CONTROLLER_P:
          actuator->state_.force_control_p_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_CONTROLLER_I:
          actuator->state_.force_control_i_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_CONTROLLER_D:
          actuator->state_.force_control_d_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_CONTROLLER_IMAX:
          actuator->state_.force_control_imax_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;
        case MOTOR_SLOW_DATA_CONTROLLER_DEADSIGN:
          tmp_value.word = status_data->motor_data_packet[index_motor_in_msg].misc;
          actuator->state_.force_control_deadband_ = static_cast<int>(tmp_value.byte[0]);
          actuator->state_.force_control_sign_ = static_cast<int>(tmp_value.byte[1]);
          break;
        case MOTOR_SLOW_DATA_CONTROLLER_FREQUENCY:
          actuator->state_.force_control_frequency_ =
            static_cast<unsigned int>(static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc));
          break;

        default:
          break;
        }
        break;

      case MOTOR_DATA_CAN_ERROR_COUNTERS:
        actuator->state_.can_error_counters =
          static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc);
        break;
      case MOTOR_DATA_PTERM:
        actuator->state_.force_control_pterm =
          static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc);
        break;
      case MOTOR_DATA_ITERM:
        actuator->state_.force_control_iterm =
          static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc);
        break;
      case MOTOR_DATA_DTERM:
        actuator->state_.force_control_dterm =
          static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].misc);
        break;

      default:
        break;
      }

      if (read_torque)
      {
        actuator->state_.force_unfiltered_ =
          static_cast<double>(static_cast<int16s>(status_data->motor_data_packet[index_motor_in_msg].torque));

#ifdef DEBUG_PUBLISHER
	if( joint_tmp->actuator_wrapper->motor_id == 8 )
        {
          msg_debug.data = static_cast<int16s>(status_data->motor_data_packet[index_motor_in_msg].torque);
          debug_publishers[4].publish(msg_debug);
        }
#endif
      }

      //Check the message to see if everything has already been received
      if (muscle_current_state == operation_mode::device_update_state::INITIALIZATION)
      {
        if (motor_data_checker->check_message(
              joint_tmp, status_data->motor_data_type,
              static_cast<int16u>(status_data->motor_data_packet[index_motor_in_msg].torque)))
        {
          motor_updater_->update_state = operation_mode::device_update_state::OPERATION;
          muscle_current_state = operation_mode::device_update_state::OPERATION;

          ROS_INFO("All motors were initialized.");
        }
      }
    }
  }


  template <class StatusType, class CommandType>
  unsigned int SrMuscleRobotLib<StatusType, CommandType>::get_muscle_pressure(int muscle_driver_id, int muscle_id, ETHERCAT_DATA_STRUCTURE_0300_PALM_EDC_STATUS *status_data)
  {
    unsigned int muscle_pressure = 0;
    int packet_offset = 0;
    int muscle_index = muscle_id;

    //Every muscle driver sends two muscle_data_packet containing pressures from 5 muscles each
    if (muscle_id >= MUSCLE_PRESSURES_PER_PACKET)
    {
      packet_offset = 1;
      muscle_index = muscle_id - MUSCLE_PRESSURES_PER_PACKET;
    }

    switch(muscle_index)
    {
      case 0:
        muscle_pressure = (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure0_H << 8)
                          + (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure0_M << 4)
                          + status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure0_L;
        break;

      case 1:
        muscle_pressure = (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure1_H << 8)
                          + (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure1_M << 4)
                          + status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure1_L;
        break;

      case 2:
        muscle_pressure = (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure2_H << 8)
                          + (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure2_M << 4)
                          + status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure2_L;
        break;

      case 3:
        muscle_pressure = (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure3_H << 8)
                          + (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure3_M << 4)
                          + status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure3_L;
        break;

      case 4:
        muscle_pressure = (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure4_H << 8)
                          + (status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure4_M << 4)
                          + status_data->muscle_data_packet[muscle_driver_id * 2 + packet_offset].packed.pressure4_L;
        break;
    }

    return muscle_pressure;
  }

  template <class StatusType, class CommandType>
  std::vector<std::pair<std::string, bool> > SrMuscleRobotLib<StatusType, CommandType>::humanize_flags(int flag)
  {
    std::vector<std::pair<std::string, bool> > flags;

    //16 is the number of flags
    for (unsigned int i = 0; i < 16; ++i)
    {
      std::pair<std::string, bool> new_flag;
      //if the flag is set add the name
      if (sr_math_utils::is_bit_mask_index_true(flag, i))
      {
        if (sr_math_utils::is_bit_mask_index_true(SERIOUS_ERROR_FLAGS, i))
          new_flag.second = true;
        else
          new_flag.second = false;

        new_flag.first = error_flag_names[i];
        flags.push_back(new_flag);
      }
    }
    return flags;
  }

  template <class StatusType, class CommandType>
  void SrMuscleRobotLib<StatusType, CommandType>::reinitialize_motors()
  {
    //Create a new MotorUpdater object
    motor_updater_ = boost::shared_ptr<generic_updater::MotorUpdater<CommandType> >(new generic_updater::MotorUpdater<CommandType>(motor_update_rate_configs_vector, operation_mode::device_update_state::INITIALIZATION));
    muscle_current_state = operation_mode::device_update_state::INITIALIZATION;
    //Initialize the motor data checker
    motor_data_checker = boost::shared_ptr<generic_updater::MotorDataChecker>(new generic_updater::MotorDataChecker(this->joints_vector, motor_updater_->initialization_configs_vector));
  }

} //end namespace

/* For the emacs weenies in the crowd.
 Local Variables:
 c-basic-offset: 2
 End:
 */
