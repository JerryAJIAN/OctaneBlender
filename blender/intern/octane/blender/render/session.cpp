/*
 * Copyright 2011, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <limits.h>

#include "session.h"
#include "buffers.h"
#include "OctaneClient.h"
#include "scene.h"
#include "kernel.h"
#include "camera.h"

#include "blender_session.h"

#include "util_math.h"
#include "util_opengl.h"
#include "util_time.h"
#include "blender_util.h"

#ifdef WIN32
#   include "BLI_winstuff.h"
#endif

OCT_NAMESPACE_BEGIN

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CONSTRUCTOR
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
Session::Session(const SessionParams& params_, const char *_out_path) : params(params_) {
    server = new ::OctaneEngine::OctaneClient;
    server->setExportType(params_.export_scene);
    server->setOutputPath(_out_path);
	//server = RenderServer::create(params.server, params.export_scene, _out_path, params.interactive);

	if(!params.interactive)
		display = NULL;
	else
		display = new DisplayBuffer(server);

	session_thread  = 0;
	scene           = 0;
    b_session       = 0;  

	start_time      = 0.0;
	reset_time      = 0.0;
	paused_time     = 0.0;

	display_outdated    = false;
	pause               = false;
} //Session()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// DESTRUCTOR
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
Session::~Session() {
	if(session_thread) {
		progress.set_cancel("Exiting");
		{
			thread_scoped_lock pause_lock(pause_mutex);
			pause = false;
		}
		pause_cond.notify_all();
		wait();
	}
	if(params.interactive && display && params.output_path != "") {
		progress.set_status("Writing Image", params.output_path);
		display->write(params.output_path);
	}
	if(display) delete display;
	if(scene) delete scene;

	delete server;
} //~Session()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Test the timeout to reset the session
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool Session::ready_to_reset() {
    return true;
    //return ((time_dt() - reset_time) > 1.0); //params.reset_timeout
} //ready_to_reset()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Runs the new rendering loop
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::start(const char* pass_name_, bool synchronous, uint32_t frame_idx_, uint32_t total_frames_) {
    pass_name       = pass_name_;
    frame_idx       = frame_idx_;
    total_frames    = total_frames_;

    if(!synchronous)
        //FIXME: kill this boost here
        session_thread = new thread(function_bind(&Session::run, this));
    else
        run();
} //start()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Render loop
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::run_render() {
	reset_time          = start_time = time_dt();
	paused_time         = 0.0;
    bool bStarted       = false;

    params.image_stat.uiCurSamples = 0;

    if(params.interactive) progress.set_start_time(start_time);

    bool is_done = false;
	while(!progress.get_cancel()) {
		if(!params.interactive) {
			// If no work left and in background mode, we can stop immediately
			if(is_done) {
                update_status_time();
				progress.set_status(string(pass_name) + " finished");
				break;
			}
		} //if(!params.interactive)
		else {
			// If in interactive mode, and we are either paused or done for now,
			// wait for pause condition notify to wake up again
			thread_scoped_lock pause_lock(pause_mutex);
			if(pause || is_done) {
				update_status_time(pause, is_done);
				while(true) {
                    if(pause) server->pauseRender(true);

					double pause_start = time_dt();
					pause_cond.wait(pause_lock);
					paused_time += time_dt() - pause_start;

				    progress.set_start_time(start_time + paused_time);
					update_status_time(pause, is_done);
					progress.set_update();

                    if(!pause) {
                        server->pauseRender(false);
                        break;
                    }
				}
			} //if(pause || is_ready)
			if(progress.get_cancel()) break;
		} //if(!params.interactive), else

		if(!is_done) {
            time_sleep(0.01);

			// Update scene on the render-server - send all changed objects
            if(!bStarted || params.interactive) update_scene_to_server(frame_idx, total_frames);

            if(!bStarted) {
                server->startRender(params.width, params.height, params.interactive ? ::OctaneEngine::OctaneClient::IMAGE_8BIT : (params.hdr_tonemapped ? ::OctaneEngine::OctaneClient::IMAGE_FLOAT_TONEMAPPED : ::OctaneEngine::OctaneClient::IMAGE_FLOAT),
                                     params.out_of_core_enabled, params.out_of_core_mem_limit, params.out_of_core_gpu_headroom); //FIXME: Perhaps the wrong place for it...
                bStarted = true;
            }

            if(!server->getServerErrorMessage().empty()) {
                progress.set_cancel("ERROR! Check console for detailed error messages.");
                server->clearServerErrorMessage();
            }
			if(progress.get_cancel()) break;

			// Buffers mutex is locked entirely while rendering each
			// sample, and released/reacquired on each iteration to allow
			// reset and draw in between
			thread_scoped_lock buffers_lock(render_buffer_mutex);

			// Update status and timing
			//update_status_time();

            update_render_buffer();
            if(!server->getServerErrorMessage().empty()) {
                progress.set_cancel("ERROR! Check console for detailed error messages.");
                server->clearServerErrorMessage();
            }

			// Update status and timing
			update_status_time();
			progress.set_update();
		} //if(!is_done)
        else {
			thread_scoped_lock buffers_lock(render_buffer_mutex);
            update_render_buffer();

            // Update status and timing
			update_status_time();
        }
		is_done = !params.interactive && (params.image_stat.uiCurSamples >= params.samples);
	} //while(!progress.get_cancel())
} //run_render()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main Render function
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::run() {
    progress.set_status("Waiting for render to start");

	if(!progress.get_cancel()) {
		progress.reset_cur_samples();
        run_render();
	}

	// Progress update
	if(progress.get_cancel()) {
        server->clear();
		progress.set_status("Cancel", progress.get_cancel_message());
    }
	else
		progress.set_update();
} //run()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Interactive drawing
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool Session::draw(BufferParams& buffer_params) {
	// Block for display buffer access
	thread_scoped_lock display_lock(display_mutex);

    // First check we already rendered something
	// then verify the buffers have the expected size, so we don't
	// draw previous results in a resized window
	if(!buffer_params.modified(display->params)) {
		return display->draw();

		if(display_outdated)// && (time_dt() - reset_time) > params.text_timeout)
			return false;

		return true;
	}
	else return false;
} //draw()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::reset_parameters(BufferParams& buffer_params) {
    if(display) {
		if(buffer_params.modified(display->params)) {
			display->reset(buffer_params);
		}
	}
	start_time      = time_dt();
	paused_time     = 0.0;

    //params.image_stat.uiCurSamples = 0;
	if(params.interactive) progress.set_start_time(start_time + paused_time);
} //reset_parameters()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reset all session data buffers
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::reset(BufferParams& buffer_params, float mb_frame_time_sampling) {
	// Block for buffer acces and reset immediately. we can't do this
	// in the thread, because we need to allocate an OpenGL buffer, and
	// that only works in the main thread
	thread_scoped_lock display_lock(display_mutex);
	thread_scoped_lock render_buffer_lock(render_buffer_mutex);

	display_outdated    = true;
	reset_time          = time_dt();

	reset_parameters(buffer_params);
    server->reset(params.export_scene, scene->kernel->uiGPUs, mb_frame_time_sampling, params.deep_image);
	pause_cond.notify_all();
} //reset()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Update render project on the render-server
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::update(BufferParams& buffer_params) {
	// Block for buffer acces and reset immediately. we can't do this
	// in the thread, because we need to allocate an OpenGL buffer, and
	// that only works in the main thread
	thread_scoped_lock display_lock(display_mutex);
	thread_scoped_lock render_buffer_lock(render_buffer_mutex);

	display_outdated    = true;
	reset_time          = time_dt();

	reset_parameters(buffer_params);
    //server->update();
	pause_cond.notify_all();
} //update()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Set max. samples value
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::set_samples(int samples) {
	if(samples != params.samples) {
		params.samples = samples;
		//{
		//	thread_scoped_lock pause_lock(pause_mutex);
        //  pause = false;
		//}
		//pause_cond.notify_all();
	}
} //set_samples()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Set current pause state
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::set_pause(bool pause_) {
	bool notify = false;
	{
		thread_scoped_lock pause_lock(pause_mutex);
		if(pause != pause_) {
			pause  = pause_;
			notify = true;
		}
	}
	if(notify) pause_cond.notify_all();
} //set_pause()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Wait for render thread finish...
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::wait() {
	session_thread->join();
	delete session_thread;

	session_thread = 0;
} //wait()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Set the parent blender session
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::set_blender_session(BlenderSession *b_session_) {
    if(!b_session_->interactive) b_session = b_session_;
	progress.set_blender_session(b_session_);

    // Render-server address
    PointerRNA oct_scene = RNA_pointer_get(&b_session_->b_scene.ptr, "octane");
    string server_addr = get_string(oct_scene, "server_address");
    if(!server_addr.length())
        fprintf(stderr, "Octane: no server address set.\n");
    else {
        if(!server->connectToServer(server_addr.c_str())) {
            if(server->getFailReason() == ::OctaneEngine::OctaneClient::FailReasons::NOT_ACTIVATED)
                fprintf(stdout, "Octane: current server activation state is: not activated.\n");
            else if(server->getFailReason() == ::OctaneEngine::OctaneClient::FailReasons::NO_CONNECTION)
                    fprintf(stderr, "Octane: can't connect to Octane server.\n");
            else if(server->getFailReason() == ::OctaneEngine::OctaneClient::FailReasons::WRONG_VERSION)
                fprintf(stderr, "Octane: wrong version of Octane server.\n");
            else
                fprintf(stderr, "Octane: can't connect to Octane server.\n");
        }
    }
} //set_blender_session()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Updates the data on the render-server
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::update_scene_to_server(uint32_t frame_idx, uint32_t total_frames, bool scene_locked) {
    if(!scene_locked) scene->mutex.lock();

	// Update camera if dimensions changed for progressive render. The camera
	// knows nothing about progressive or cropped rendering, it just gets the
	// image dimensions passed in
	Camera  *cam    = scene->camera;
	int     width   = params.width;
	int     height  = params.height;

	if(width != cam->width || height != cam->height) {
		cam->width  = width;
		cam->height = height;
		cam->tag_update();
	}

	// Update scene
	if(params.export_scene != ::OctaneEngine::OctaneClient::SceneExportTypes::NONE || scene->need_update()) {
		progress.set_status("Updating Scene");
        scene->server_update(server, progress, params.interactive, frame_idx, total_frames);
	}

    if(!scene_locked) scene->mutex.unlock();
} //update_scene_to_device()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Update status string with current render info
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::update_status_time(bool show_pause, bool show_done) {
	string status, substatus;

    if(server->checkServerConnection()) {
        if(params.image_stat.uiCurSamples > 0) {
            char szSamples[16];
            unsigned long ulSPSdivider;
            if(params.image_stat.fSPS < 999999) {
                ulSPSdivider = 1000;
#ifndef WIN32
                ::snprintf(szSamples, 16, "%.2f Ks/sec", params.image_stat.fSPS/ulSPSdivider);
#else
                ::snprintf(szSamples, 16, "%.2f Ks/sec", params.image_stat.fSPS/ulSPSdivider);
#endif
            }
            else {
                ulSPSdivider = 1000000;
#ifndef WIN32
                ::snprintf(szSamples, 16, "%.2f Ms/sec", params.image_stat.fSPS/ulSPSdivider);
#else
                ::snprintf(szSamples, 16, "%.2f Ms/sec", params.image_stat.fSPS/ulSPSdivider);
#endif
            }

            if(params.samples == INT_MAX) {
                if(params.image_stat.uiNetGPUs > 0)
                    substatus = string_printf("Sample %d, %s | Mem: %dM/%dM/%dM, Meshes: %d, Tris: %d | Tex: ( Rgb32: %d, Rgb64: %d, grey8: %d, grey16: %d ) | Net GPUs: %u/%u",
                        params.image_stat.uiCurSamples, szSamples, params.image_stat.ulVramUsed/1000000, params.image_stat.ulVramFree/1000000, params.image_stat.ulVramTotal/1000000, params.image_stat.uiMeshesCnt, params.image_stat.uiTrianglesCnt,
                        params.image_stat.uiRgb32Cnt, params.image_stat.uiRgb64Cnt, params.image_stat.uiGrey8Cnt, params.image_stat.uiGrey16Cnt, params.image_stat.uiNetGPUsUsed, params.image_stat.uiNetGPUs);
                else
                    substatus = string_printf("Sample %d, %s | Mem: %dM/%dM/%dM, Meshes: %d, Tris: %d | Tex: ( Rgb32: %d, Rgb64: %d, grey8: %d, grey16: %d ) | No net GPUs",
                        params.image_stat.uiCurSamples, szSamples, params.image_stat.ulVramUsed / 1000000, params.image_stat.ulVramFree / 1000000, params.image_stat.ulVramTotal / 1000000, params.image_stat.uiMeshesCnt, params.image_stat.uiTrianglesCnt,
                        params.image_stat.uiRgb32Cnt, params.image_stat.uiRgb64Cnt, params.image_stat.uiGrey8Cnt, params.image_stat.uiGrey16Cnt);
            }
	        else {
                if(params.image_stat.uiNetGPUs > 0)
                    substatus = string_printf("Sample %d/%d, %s | Mem: %dM/%dM/%dM, Meshes: %d, Tris: %d | Tex: ( Rgb32: %d, Rgb64: %d, grey8: %d, grey16: %d ) | Net GPUs: %u/%u",
                        params.image_stat.uiCurSamples, params.samples, szSamples, params.image_stat.ulVramUsed/1000000, params.image_stat.ulVramFree / 1000000, params.image_stat.ulVramTotal/1000000, params.image_stat.uiMeshesCnt, params.image_stat.uiTrianglesCnt,
                        params.image_stat.uiRgb32Cnt, params.image_stat.uiRgb64Cnt, params.image_stat.uiGrey8Cnt, params.image_stat.uiGrey16Cnt, params.image_stat.uiNetGPUsUsed, params.image_stat.uiNetGPUs);
                else
                    substatus = string_printf("Sample %d/%d, %s | Mem: %dM/%dM/%dM, Meshes: %d, Tris: %d | Tex: ( Rgb32: %d, Rgb64: %d, grey8: %d, grey16: %d ) | No net GPUs",
                        params.image_stat.uiCurSamples, params.samples, szSamples, params.image_stat.ulVramUsed / 1000000, params.image_stat.ulVramFree / 1000000, params.image_stat.ulVramTotal / 1000000, params.image_stat.uiMeshesCnt, params.image_stat.uiTrianglesCnt,
                        params.image_stat.uiRgb32Cnt, params.image_stat.uiRgb64Cnt, params.image_stat.uiGrey8Cnt, params.image_stat.uiGrey16Cnt);
            }
            if(params.image_stat.iExpiryTime == 0) {
                substatus = substatus + " | SUBSCRIPTION IS EXPIRED!";
            }
            else if(params.image_stat.iExpiryTime > 0 && params.image_stat.iExpiryTime < (3600 * 48)) {
                substatus = substatus + string_printf(" | Subscription expires in %d:%.2d:%.2d", params.image_stat.iExpiryTime / 3600, (params.image_stat.iExpiryTime % 3600) / 60, (params.image_stat.iExpiryTime % 3600) % 60);
            }
        }
        else
            substatus = "Waiting for image...";
    	
        if(b_session && !b_session->interactive) status = pass_name;
        else status = "Interactive";

        if(show_pause)
	        status += " - Paused";
        else if(show_done)
	        status += " - Done";
        else
	        status += " - Rendering";
    }
    else {
        switch(server->getFailReason()) {
            case ::OctaneEngine::OctaneClient::FailReasons::NO_CONNECTION:
            status = "Not connected";
            substatus = string("No Render-server at address \"") + server->getServerInfo().sNetAddress + "\"";
            break;
        case ::OctaneEngine::OctaneClient::FailReasons::WRONG_VERSION:
            status = "Wrong version";
            substatus = string("Wrong Render-server version at address \"") + server->getServerInfo().sNetAddress + "\"";
            break;
        case ::OctaneEngine::OctaneClient::FailReasons::NOT_ACTIVATED:
            status = "Not activated";
            substatus = string("Render-server at address \"") + server->getServerInfo().sNetAddress + "\" is not activated";
            break;
        default:
            status = "Server error";
            substatus = string("Error in Render-server at address \"") + server->getServerInfo().sNetAddress + "\"";
            break;
        }
    }
	progress.set_status(status, substatus);
	progress.refresh_cur_info();
} //update_status_time()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Refresh the render-buffer and render-view with the new image from server
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::update_render_buffer() {
    if(progress.get_cancel()) return;

    ::Octane::RenderPassId passId = scene->passes->oct_node->bUsePasses ? scene->passes->oct_node->curPassType : ::Octane::RENDER_PASS_BEAUTY;
    if(params.interactive && !server->downloadImageBuffer(params.image_stat, params.interactive ? ::OctaneEngine::OctaneClient::IMAGE_8BIT : (params.hdr_tonemapped ? ::OctaneEngine::OctaneClient::IMAGE_FLOAT_TONEMAPPED : ::OctaneEngine::OctaneClient::IMAGE_FLOAT), passId) && b_session) {
        if(progress.get_cancel()) return;
        if(!params.interactive) update_img_sample();
        passId = scene->passes->oct_node->bUsePasses ? scene->passes->oct_node->curPassType : ::Octane::RENDER_PASS_BEAUTY;
        server->downloadImageBuffer(params.image_stat, params.interactive ? ::OctaneEngine::OctaneClient::IMAGE_8BIT : (params.hdr_tonemapped ? ::OctaneEngine::OctaneClient::IMAGE_FLOAT_TONEMAPPED : ::OctaneEngine::OctaneClient::IMAGE_FLOAT), passId);
    }
    if(progress.get_cancel()) return;
    if(!params.interactive) update_img_sample();
} //update_render_buffer()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Refresh the render-view with new image from render-buffer
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Session::update_img_sample() {
    //Only for NON-INTERACTIVE session
	if(b_session) {
    	thread_scoped_lock img_lock(img_mutex);
		//TODO: optimize this by making it thread safe and removing lock
		b_session->update_render_img();
	}
	update_status_time();
} //update_img_sample()

OCT_NAMESPACE_END

