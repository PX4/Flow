/****************************************************************************
 *
 *   Copyright (C) 2013 PX4 Development Team. All rights reserved.
 *   Author: Laurens Mackay <mackayl@student.ethz.ch>
 *   		 Dominik Honegger <dominik.honegger@inf.ethz.ch>
 *   		 Petri Tanskanen <tpetri@inf.ethz.ch>
 *   		 Samuel Zihlmann <samuezih@ee.ethz.ch>
 *           Simon Laube <simon@leitwert.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <px4_config.h>
#include <bsp/board.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "stm32f4xx_conf.h"
#include "stm32f4xx.h"

#include "no_warnings.h"
#include "mavlink_bridge_header.h"
#include <mavlink.h>
#include "settings.h"
#include "result_accumulator.h"
#include "flow.h"
#include "timer.h"
#include "dcmi.h"
#include "mt9v034.h"
#include "gyro.h"
#include "fmu_comm.h"
#include "usart.h"
#include "distance.h"
#include "communication.h"
#include "usbd_cdc_core.h"
#include "usbd_usr.h"
#include "usbd_desc.h"
#include "usbd_cdc_vcp.h"
#include "main.h"
#include "hrt.h"
#include <px4_macros.h>

//#define CONFIG_USE_PROBES
#include <bsp/probes.h>


/* coprocessor control register (fpu) */
#ifndef SCB_CPACR
#define SCB_CPACR (*((uint32_t*) (((0xE000E000UL) + 0x0D00UL) + 0x088)))
#endif


__ALIGN_BEGIN USB_OTG_CORE_HANDLE  USB_OTG_dev __ALIGN_END;

#define FLOW_IMAGE_SIZE (64)

static void check_for_frame(void);
static float latest_ground_distance(void);

/* timer constants */
#define DISTANCE_POLL_MS	 	100	/* steps in milliseconds ticks */
#define SYSTEM_STATE_MS		1000/* steps in milliseconds ticks */
#define PARAMS_MS			100	/* steps in milliseconds ticks */
#define LPOS_TIMER_COUNT 	100	/* steps in milliseconds ticks */

static camera_ctx cam_ctx;
static camera_img_param img_stream_param;

static void distance_update_fn(void) {
	static bool state = 0;
	if (state == 0) {
		distance_trigger();
		state = 1;
	} else {
		distance_readback();
		state = 0;
	}
}

static void system_state_send_fn(void) {
	/* every second */
	if (FLOAT_AS_BOOL(global_data.param[PARAM_SYSTEM_SEND_STATE]))
	{
		communication_system_state_send();
	}
}

static void system_receive_fn(void) {
	/* test every 0.5s */
	communication_receive();
	communication_receive_usb();
}

const camera_image_buffer *previous_image = NULL;

static void mavlink_send_image(const camera_image_buffer *image) {
	uint32_t img_size = (uint32_t)image->param.p.size.x * (uint32_t)image->param.p.size.y;
	mavlink_msg_data_transmission_handshake_send(
			MAVLINK_COMM_2,
			MAVLINK_DATA_STREAM_IMG_RAW8U,
			img_size,
			image->param.p.size.x,
			image->param.p.size.y,
			img_size / MAVLINK_MSG_ENCAPSULATED_DATA_FIELD_DATA_LEN + 1,
			MAVLINK_MSG_ENCAPSULATED_DATA_FIELD_DATA_LEN,
			100);
	uint16_t frame = 0;
	for (frame = 0; frame < img_size / MAVLINK_MSG_ENCAPSULATED_DATA_FIELD_DATA_LEN + 1; frame++) {
		mavlink_msg_encapsulated_data_send(MAVLINK_COMM_2, frame, &((uint8_t *)image->buffer)[frame * MAVLINK_MSG_ENCAPSULATED_DATA_FIELD_DATA_LEN]);
	}
}

static void send_video_fn(void) {
	/* update the rate */
	timer_update(send_video_fn, global_data.param[PARAM_VIDEO_RATE]);

	if (previous_image == NULL) return;
	
	/*  transmit raw 8-bit image */
	if (FLOAT_AS_BOOL(global_data.param[PARAM_USB_SEND_VIDEO]))
	{
		mavlink_send_image(previous_image);
	}
}

static void send_params_fn(void) {
	communication_parameter_send();
}

/*void switch_params_fn(void) {
	switch (img_stream_param.binning) {
		case 1: img_stream_param.binning = 4; break;
		case 2: img_stream_param.binning = 1; break;
		case 4: img_stream_param.binning = 2; break;
	}
	camera_img_stream_schedule_param_change(&cam_ctx, &img_stream_param);
}*/

void notify_changed_camera_parameters(void) {
	camera_reconfigure_general(&cam_ctx);
}

#define FLOW_IMAGE_SIZE (64)

static uint8_t image_buffer_8bit_1[FLOW_IMAGE_SIZE * FLOW_IMAGE_SIZE] __attribute__((section(".ccm")));
static uint8_t image_buffer_8bit_2[FLOW_IMAGE_SIZE * FLOW_IMAGE_SIZE] __attribute__((section(".ccm")));
static uint8_t image_buffer_8bit_3[FLOW_IMAGE_SIZE * FLOW_IMAGE_SIZE] __attribute__((section(".ccm")));
static uint8_t image_buffer_8bit_4[FLOW_IMAGE_SIZE * FLOW_IMAGE_SIZE] __attribute__((section(".ccm")));
static uint8_t image_buffer_8bit_5[FLOW_IMAGE_SIZE * FLOW_IMAGE_SIZE] __attribute__((section(".ccm")));

static flow_klt_image flow_klt_images[2] __attribute__((section(".ccm")));

/* variables */
uint32_t counter = 0;

result_accumulator_ctx mavlink_accumulator;
uint32_t fps_timing_start;
uint16_t fps_counter = 0;
uint16_t fps_skipped_counter = 0;

uint32_t last_frame_index = 0;
uint32_t last_processed_frame_timestamp;

/**
  * @brief  Main function.
  */
int main(void)
{
	__enable_irq();

	/* load settings and parameters */
	global_data_reset_param_defaults();
	global_data_reset();
	PROBE_INIT();
	
	board_led_initialize();
	
	/* enable FPU on Cortex-M4F core */
	SCB_CPACR |= ((3UL << 10 * 2) | (3UL << 11 * 2)); /* set CP10 Full Access and set CP11 Full Access */

	/* init timers */
	timer_init();

	/* init usb */
	USBD_Init(	&USB_OTG_dev,
				USB_OTG_FS_CORE_ID,
				&USR_desc,
				&USBD_CDC_cb,
				&USR_cb);

	/* init mavlink */
	communication_init();

	/* initialize camera: */
	img_stream_param.size.x = FLOW_IMAGE_SIZE;
	img_stream_param.size.y = FLOW_IMAGE_SIZE;
	img_stream_param.binning = 4;
	{
		camera_image_buffer buffers[5] = {
			BuildCameraImageBuffer(image_buffer_8bit_1),
			BuildCameraImageBuffer(image_buffer_8bit_2),
			BuildCameraImageBuffer(image_buffer_8bit_3),
			BuildCameraImageBuffer(image_buffer_8bit_4),
			BuildCameraImageBuffer(image_buffer_8bit_5)
		};
		camera_init(&cam_ctx, mt9v034_get_sensor_interface(), dcmi_get_transport_interface(), 
					mt9v034_get_clks_per_row(64, 4) * 1, mt9v034_get_clks_per_row(64, 4) * 128, 2.0,
					&img_stream_param, buffers, 5);
	}

	/* gyro config */
	gyro_config();

	/* usart config*/
	usart_init();
	fmu_comm_init();
	distance_init();

	/* reset/start timers */
	timer_register(distance_update_fn, DISTANCE_POLL_MS/2);
	timer_register(system_state_send_fn, SYSTEM_STATE_MS);
	timer_register(system_receive_fn, SYSTEM_STATE_MS / 2);
	timer_register(send_params_fn, PARAMS_MS);
	timer_register(send_video_fn, global_data.param[PARAM_VIDEO_RATE]);
	//timer_register(switch_params_fn, 2000);

	result_accumulator_init(&mavlink_accumulator);
	fps_timing_start = last_processed_frame_timestamp = get_boot_time_us();
	
	/* main loop */
	while (1)
	{
		/* check timers */
		timer_check();
		fmu_comm_run();
		check_for_frame();
	}
}

static float latest_ground_distance(void) {
	float distance_filtered, distance_raw;
	bool distance_valid = distance_read(&distance_filtered, &distance_raw);
	if (!distance_valid) {
		return -1;
	} else if(FLOAT_AS_BOOL(global_data.param[PARAM_SONAR_FILTERED])) {
		return distance_filtered;
	}	else {
		return distance_raw;
	}
}

static void check_for_frame(void) {
	/* get recent images */
	camera_image_buffer *frames[2];
	if (camera_img_stream_get_buffers(&cam_ctx, frames, 2, true)) {
		/* new gyroscope data */
		float x_rate_sensor, y_rate_sensor, z_rate_sensor;
		int16_t gyro_temp;
		gyro_read(&x_rate_sensor, &y_rate_sensor, &z_rate_sensor,&gyro_temp);

		/* gyroscope coordinate transformation to flow sensor coordinates */
		float x_rate =   y_rate_sensor; // change x and y rates
		float y_rate = - x_rate_sensor;
		float z_rate =   z_rate_sensor; // z is correct
		
		bool use_klt = !FLOAT_EQ_INT(global_data.param[PARAM_ALGORITHM_CHOICE], 0);
		
		uint32_t start_computations = get_boot_time_us();
		
		int frame_delta = ((int32_t)frames[0]->frame_number - (int32_t)last_frame_index);
		last_frame_index = frames[0]->frame_number;
		fps_skipped_counter += frame_delta - 1;
		
		flow_klt_image *klt_images[2] = {NULL, NULL};
		{
			/* make sure that the new images get the correct treatment */
			/* this algorithm will still work if both images are new */
			int i;
			bool used_klt_image[2] = {false, false};
			for (i = 0; i < 2; ++i) {
				if (frames[i]->frame_number != frames[i]->meta) {
					/* update meta data to mark it as an up-to date image: */
					frames[i]->meta = frames[i]->frame_number;
				} else {
					// the image has the preprocessing already applied.
					if (use_klt) {
						int j;
						/* find the klt image that matches: */
						for (j = 0; j < 2; ++j) {
							if (flow_klt_images[j].meta == frames[i]->frame_number) {
								used_klt_image[j] = true;
								klt_images[i] = &flow_klt_images[j];
							}
						}
					}
				}
			}
			if (use_klt) {
				/* only for KLT: */
				/* preprocess the images if they are not yet preprocessed */
				for (i = 0; i < 2; ++i) {
					if (klt_images[i] == NULL) {
						// need processing. find unused KLT image:
						int j;
						for (j = 0; j < 2; ++j) {
							if (!used_klt_image[j]) {
								used_klt_image[j] = true;
								klt_images[i] = &flow_klt_images[j];
								break;
							}
						}
						klt_preprocess_image(frames[i]->buffer, klt_images[i]);
						klt_images[i]->meta = frames[i]->frame_number;
					}
				}
			}
		}
		
		float frame_dt   = calculate_time_delta_us(frames[0]->timestamp, frames[1]->timestamp)           * 0.000001f;
		float dropped_dt = calculate_time_delta_us(frames[1]->timestamp, last_processed_frame_timestamp) * 0.000001f;
		last_processed_frame_timestamp = frames[0]->timestamp;

		/* calculate focal_length in pixel */
		const float focal_length_px = (global_data.param[PARAM_FOCAL_LENGTH_MM]) / 
									  ((float)frames[0]->param.p.binning * 0.006f);	// pixel-size: 6um

		/* extract the raw flow from the images: */
		flow_raw_result flow_rslt[32];
		uint16_t flow_rslt_count = 0;
		/* make sure both images are taken with same binning mode: */
		if (frames[0]->param.p.binning == frames[1]->param.p.binning) {
			/* compute gyro rate in pixels and change to image coordinates */
			float x_rate_px = - y_rate * (focal_length_px * frame_dt);
			float y_rate_px =   x_rate * (focal_length_px * frame_dt);
			float z_rate_fr = - z_rate * frame_dt;

			/* compute optical flow in pixels */
			if (!use_klt) {
				flow_rslt_count = compute_flow(frames[1]->buffer, frames[0]->buffer, x_rate_px, y_rate_px, z_rate_fr, 
											   flow_rslt, sizeof(flow_rslt) / sizeof(flow_rslt[0]));
			} else {
				flow_rslt_count =  compute_klt(klt_images[1], klt_images[0],         x_rate_px, y_rate_px, z_rate_fr, 
											   flow_rslt, sizeof(flow_rslt) / sizeof(flow_rslt[0]));
			}
		} else {
			/* no result for this frame. */
			flow_rslt_count = 0;
		}
		/* determine velocity capability: */
		float flow_mv_cap;
		if (!use_klt) {
			flow_mv_cap = get_flow_capability();
		} else {
			flow_mv_cap = get_flow_klt_capability();
		}

		/* calculate flow value from the raw results */
		float pixel_flow_x;
		float pixel_flow_y;
		float outlier_threshold = global_data.param[PARAM_ALGORITHM_OUTLIER_THR_RATIO];
		float min_outlier_threshold = 0;
		if(FLOAT_EQ_INT(global_data.param[PARAM_ALGORITHM_CHOICE], 0))
		{
			min_outlier_threshold = global_data.param[PARAM_ALGORITHM_OUTLIER_THR_BLOCK];
		}else
		{
			min_outlier_threshold = global_data.param[PARAM_ALGORITHM_OUTLIER_THR_KLT];
		}
		uint8_t qual = flow_extract_result(flow_rslt, flow_rslt_count, &pixel_flow_x, &pixel_flow_y, 
							outlier_threshold,  min_outlier_threshold);

		/* create flow image if needed (previous_image is not needed anymore)
		 * -> can be used for debugging purpose
		 */
		previous_image = frames[1];
		if (FLOAT_AS_BOOL(global_data.param[PARAM_USB_SEND_VIDEO]))
		{
			uint16_t frame_size = global_data.param[PARAM_IMAGE_WIDTH];
			uint8_t *prev_img = previous_image->buffer;
			for (int i = 0; i < flow_rslt_count; i++) {
				if (flow_rslt[i].quality > 0) {
					prev_img[flow_rslt[i].at_y * frame_size + flow_rslt[i].at_x] = 255;
					int ofs = (int)(flow_rslt[i].at_y + flow_rslt[i].y * 2 + 0.5f) * frame_size + (int)(flow_rslt[i].at_x + flow_rslt[i].x * 2 + 0.5f);
					if (ofs >= 0 && ofs < frame_size * frame_size) {
						prev_img[ofs] = 200;
					}
				}
			}
		}

		/* return the image buffers */
		camera_img_stream_return_buffers(&cam_ctx, frames, 2);
		
		result_accumulator_frame frame_data = {
			.dt = frame_dt,
			.dropped_dt = dropped_dt,
			.x_rate = x_rate,
			.y_rate = y_rate,
			.z_rate = z_rate,
			.gyro_temp = gyro_temp,
			.qual = qual,
			.pixel_flow_x = pixel_flow_x,
			.pixel_flow_y = pixel_flow_y,
			.rad_per_pixel = 1.0f / focal_length_px,
			.ground_distance = latest_ground_distance(),
			.distance_age = get_time_delta_us(get_distance_measure_time()),
			.max_px_frame = flow_mv_cap,
		};

		/* update I2C transmit buffer */
		fmu_comm_update(&frame_data);
		result_accumulator_feed(&mavlink_accumulator, &frame_data);

		uint32_t computation_time_us = get_time_delta_us(start_computations);

		counter++;
		fps_counter++;

        /* serial mavlink  + usb mavlink output throttled */
		if (counter % (uint32_t)global_data.param[PARAM_FLOW_SERIAL_THROTTLE_FACTOR] == 0)//throttling factor
		{
			float fps = 0;
			float fps_skip = 0;
			if (fps_counter + fps_skipped_counter > 100) {
				uint32_t dt = get_time_delta_us(fps_timing_start);
				fps_timing_start += dt;
				fps = (float)fps_counter / ((float)dt * 1e-6f);
				fps_skip = (float)fps_skipped_counter / ((float)dt * 1e-6f);
				fps_counter = 0;
				fps_skipped_counter = 0;

				mavlink_msg_debug_vect_send(MAVLINK_COMM_2, "TIMING", get_boot_time_us(), computation_time_us, fps, fps_skip);
			}
			mavlink_msg_debug_vect_send(MAVLINK_COMM_2, "EXPOSURE", get_boot_time_us(), 
					frames[0]->param.exposure, frames[0]->param.analog_gain, cam_ctx.last_brightness);
			
			/* calculate the output values */
			result_accumulator_output_flow output_flow;
			result_accumulator_output_flow_rad output_flow_rad;
			int min_valid_ratio = global_data.param[PARAM_ALGORITHM_MIN_VALID_RATIO];
			result_accumulator_calculate_output_flow(&mavlink_accumulator, min_valid_ratio, &output_flow);
			result_accumulator_calculate_output_flow_rad(&mavlink_accumulator, min_valid_ratio, &output_flow_rad);
			
			board_led_status_update(output_flow.quality / 255.0);

			// send flow
			mavlink_msg_optical_flow_send(MAVLINK_COMM_0, get_boot_time_us(), global_data.param[PARAM_SENSOR_ID],
					output_flow.flow_x, output_flow.flow_y,
					output_flow.flow_comp_m_x, output_flow.flow_comp_m_y, 
					output_flow.quality, output_flow.ground_distance);

			mavlink_msg_optical_flow_rad_send(MAVLINK_COMM_0, get_boot_time_us(), global_data.param[PARAM_SENSOR_ID],
					output_flow_rad.integration_time, 
					output_flow_rad.integrated_x, output_flow_rad.integrated_y,
					output_flow_rad.integrated_xgyro, output_flow_rad.integrated_ygyro, output_flow_rad.integrated_zgyro,
					output_flow_rad.temperature, output_flow_rad.quality,
					output_flow_rad.time_delta_distance_us,output_flow_rad.ground_distance);

			if (FLOAT_AS_BOOL(global_data.param[PARAM_USB_SEND_FLOW]) && (output_flow.quality > 0 || FLOAT_AS_BOOL(global_data.param[PARAM_USB_SEND_QUAL_0])))
			{
				mavlink_msg_optical_flow_send(MAVLINK_COMM_2, get_boot_time_us(), global_data.param[PARAM_SENSOR_ID],
						output_flow.flow_x, output_flow.flow_y,
						output_flow.flow_comp_m_x, output_flow.flow_comp_m_y, 
						output_flow.quality, output_flow.ground_distance);

				mavlink_msg_optical_flow_rad_send(MAVLINK_COMM_2, get_boot_time_us(), global_data.param[PARAM_SENSOR_ID],
						output_flow_rad.integration_time, 
						output_flow_rad.integrated_x, output_flow_rad.integrated_y,
						output_flow_rad.integrated_xgyro, output_flow_rad.integrated_ygyro, output_flow_rad.integrated_zgyro,
						output_flow_rad.temperature, output_flow_rad.quality,
						output_flow_rad.time_delta_distance_us,output_flow_rad.ground_distance);
			}

			if(FLOAT_AS_BOOL(global_data.param[PARAM_USB_SEND_GYRO]))
			{
				mavlink_msg_debug_vect_send(MAVLINK_COMM_2, "GYRO", get_boot_time_us(), x_rate, y_rate, z_rate);
			}

			result_accumulator_reset(&mavlink_accumulator);
		}

		/* forward flow from other sensors */
		if (counter % 2)
		{
			communication_receive_forward();
		}
	}
}
