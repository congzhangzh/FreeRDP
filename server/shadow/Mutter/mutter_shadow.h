/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2026 Cong Zhang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FREERDP_SERVER_SHADOW_MUTTER_SHADOW_H
#define FREERDP_SERVER_SHADOW_MUTTER_SHADOW_H

#include <freerdp/server/shadow.h>

#include "shadow_pipewire_audio.h"
#include <freerdp/codec/audio.h>

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/stream.h>

#include <systemd/sd-bus.h>
#include <pipewire/pipewire.h>
#include <libei.h>

#define MUTTER_SHADOW_MAX_MONITORS 16
#define MUTTER_SHADOW_CONNECTOR_SIZE 64

typedef struct mutter_shadow_subsystem mutterShadowSubsystem;
typedef struct mutter_output mutterOutput;

/* A monitor the server records: the shared monitor, or one virtual monitor per client
 * monitor. Its stream is copied to its place (x, y) in the desktop, the shadow surface.
 * Only the subsystem thread uses it, but for mappingId (under eiLock). */
struct mutter_output
{
	mutterShadowSubsystem* mutter;
	UINT32 index;
	BOOL used;

	/* the screen cast stream on the session bus */
	char* streamPath;
	sd_bus_slot* streamAddedSlot;
	char* mappingId; /* ties the stream to its libei region */
	UINT32 nodeId;
	BOOL haveNode;
	BOOL streamGone; /* Mutter ended the stream (the monitor's layout changed) */
	UINT64 streamGoneAt;
	BOOL awaitNode; /* a stream recorded again waits for its PipeWire node */
	BOOL reconnect; /* that node is there: connect the PipeWire stream to it */

	/* PipeWire: width and height are the stream's */
	struct pw_stream* stream;
	struct spa_hook streamListener;
	UINT32 width;
	UINT32 height;
	UINT32 format; /* FreeRDP pixel format of the stream */
	BOOL haveFormat;
	BOOL fullPaint;

	/* its place in the desktop, and the size asked for a virtual monitor */
	INT32 x;
	INT32 y;
	BOOL primary;
	UINT32 wantWidth;
	UINT32 wantHeight;
	/* its monitor in GNOME (Meta-<n>), and when it was made, to tell which one that is */
	char connector[MUTTER_SHADOW_CONNECTOR_SIZE];
	UINT64 createdSeq;

	/* the check for changes Mutter left out of its damage (mutter_repair_missed_damage) */
	BOOL damageMissSeen;
	UINT64 damageFrames;
	UINT64 damageTiles;
	UINT64 damageLogged;
};

struct mutter_shadow_subsystem
{
	rdpShadowSubsystem common;

	/* the session bus, and the remote desktop and screen cast sessions on it;
	 * used by the init and then only by the subsystem thread */
	sd_bus* bus;
	sd_bus_slot* streamAddedSlot;
	sd_bus_slot* closedSlot;
	sd_bus_slot* ownerSlot; /* GNOME Shell leaving the bus */
	char* rdSessionPath;
	char* scSessionPath;
	BOOL closed;
	BOOL shellGone;
	BOOL monitorGone; /* the shared monitor left the layout */
	BOOL closedLogged;

	/* FREERDP_SHADOW_MUTTER_VIRTUAL=<w>x<h>: a monitor of its own for this server
	 * (RecordVirtual), for a session started without one (a headless GNOME),
	 * instead of a mirror of an existing monitor */
	BOOL virtualMonitor;
	UINT32 virtualWidth;
	UINT32 virtualHeight;

	/* the monitors as Mutter lays them out, and their connectors */
	char connectors[MUTTER_SHADOW_MAX_MONITORS][MUTTER_SHADOW_CONNECTOR_SIZE];

	/* PipeWire, only in the subsystem thread once started: the outputs' streams.
	 * width and height are the desktop's, the box around the outputs, which the
	 * surface follows */
	struct pw_loop* loop;
	struct pw_context* context;
	struct pw_core* core;
	struct spa_hook coreListener;
	struct pw_registry* registry; /* tells when Mutter's node goes away */
	struct spa_hook registryListener;
	mutterOutput outputs[MUTTER_SHADOW_MAX_MONITORS];
	UINT32 outputCount;
	UINT64 outputSeq;
	/* GNOME gets the client's layout (DisplayConfig.ApplyMonitorsConfig) once every output
	 * has its stream; not when GNOME has monitors of its own, which a layout without them
	 * would turn off */
	BOOL layoutPending;
	UINT64 layoutAt;
	BOOL foreignKnown;
	UINT32 foreignCount;
	char foreign[MUTTER_SHADOW_MAX_MONITORS][MUTTER_SHADOW_CONNECTOR_SIZE];
	UINT32 width;
	UINT32 height;
	BOOL started; /* the remote desktop session */

	/* the check for changes Mutter left out of its damage (mutter_repair_missed_damage) */
	BOOL damageCheck;

	/* libei, under eiLock: the client threads post input, the subsystem
	 * thread reads its events */
	CRITICAL_SECTION eiLock;
	struct ei* ei;
	struct ei_device* keyboard;
	struct ei_device* pointer;
	BOOL keyboardReady;
	BOOL pointerReady;
	uint32_t sequence;
	uint32_t lockedMods;
	BOOL lockedModsKnown;
	UINT64 pointerInput; /* when a client last moved the pointer */

	/* the cursor the clients draw: its last shape and position */
	UINT32 cursorHash;
	BOOL cursorKnown;
	BOOL cursorResend;
	BYTE* cursorPixels; /* the last shape, RGBA: Mutter only sends a shape when it changes */
	UINT32 cursorWidth;
	UINT32 cursorHeight;
	UINT32 cursorXHot;
	UINT32 cursorYHot;
	INT32 cursorX;
	INT32 cursorY;

	ShadowPipewireAudio* audio; /* nullptr without PipeWire */

	/* clipboard: D-Bus on the subsystem thread; an application may stall the pipes
	 * of a transfer, so the data moves on a thread of its own */
	BOOL clipboardEnabled;
	sd_bus_slot* selectionOwnerSlot;
	sd_bus_slot* selectionTransferSlot;
	wMessageQueue* clipboardJobs;
	wArrayList* clipboardPipes;
	HANDLE clipboardThread;

	volatile LONG clients;

	void* volatile sizeClient; /* the newest client, only ever compared */

	HANDLE thread;
	HANDLE stopEvent;
};

#endif /* FREERDP_SERVER_SHADOW_MUTTER_SHADOW_H */
