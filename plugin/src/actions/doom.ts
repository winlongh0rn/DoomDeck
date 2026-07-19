import {
	action,
	KeyDownEvent,
	KeyUpEvent,
	SingletonAction,
	WillAppearEvent,
	WillDisappearEvent
} from "@elgato/streamdeck";

import { createConnection, type Socket } from "node:net";

import { FrameRenderer } from "../renderer/FrameRenderer";

const FRAME_PIPE_PATH = String.raw`\\.\pipe\DoomDeckFrames`;
const INPUT_PIPE_PATH = String.raw`\\.\pipe\DoomDeckInput`;

const HEADER_SIZE = 32;
const PROTOCOL_MAGIC = "DDRF";
const PROTOCOL_VERSION = 1;
const RECONNECT_DELAY_MS = 1000;
const MAX_FRAME_PAYLOAD = 16 * 1024 * 1024;

interface VisibleKey {
	action: {
		setImage(image: string): Promise<void>;
		setTitle(title: string): Promise<void>;
	};
	column: number;
	row: number;
}

interface FrameHeader {
	width: number;
	height: number;
	channels: 1 | 2 | 3 | 4;
	payloadSize: number;
	frameNumber: bigint;
}

interface RawFrame extends FrameHeader {
	pixels: Buffer;
}

/*
 * The native helper expects two-byte input packets:
 *
 * byte 0: 0 = key down, 1 = key up
 * byte 1: DoomControl value below
 */
enum InputAction {
	Press = 0,
	Release = 1
}

enum DoomControl {
	PreviousWeapon = 0,
	Run = 1,
	Fire = 2,
	Use = 3,
	NextWeapon = 4,

	Map = 5,
	TurnLeft = 6,
	Forward = 7,
	TurnRight = 8,
	Escape = 9,

	QuickSave = 10,
	StrafeLeft = 11,
	Backward = 12,
	StrafeRight = 13,
	QuickLoad = 14
}

/*
 * Physical 5x3 Stream Deck layout:
 *
 * ┌──────────────┬───────────┬───────────┬───────────┬──────────────┐
 * │ Prev Weapon  │ Run       │ Fire      │ Use       │ Next Weapon  │
 * │ Q            │ Shift     │ Ctrl      │ Space     │ E            │
 * ├──────────────┼───────────┼───────────┼───────────┼──────────────┤
 * │ Map          │ Turn Left │ Forward   │ Turn Right│ Escape       │
 * │ Tab          │ Left      │ Up        │ Right     │ Esc          │
 * ├──────────────┼───────────┼───────────┼───────────┼──────────────┤
 * │ Quick Save   │ Strafe L  │ Backward  │ Strafe R  │ Quick Load   │
 * │ F6           │ Comma     │ Down      │ Period    │ F9           │
 * └──────────────┴───────────┴───────────┴───────────┴──────────────┘
 */
const CONTROL_BY_POSITION: readonly (readonly DoomControl[])[] = [
	[
		DoomControl.PreviousWeapon,
		DoomControl.Run,
		DoomControl.Fire,
		DoomControl.Use,
		DoomControl.NextWeapon
	],
	[
		DoomControl.Map,
		DoomControl.TurnLeft,
		DoomControl.Forward,
		DoomControl.TurnRight,
		DoomControl.Escape
	],
	[
		DoomControl.QuickSave,
		DoomControl.StrafeLeft,
		DoomControl.Backward,
		DoomControl.StrafeRight,
		DoomControl.QuickLoad
	]
];

const renderer = new FrameRenderer();
const visibleKeys = new Map<string, VisibleKey>();

let frameSocket: Socket | undefined;
let frameReconnectTimer: ReturnType<typeof setTimeout> | undefined;
let framePipeClientStarted = false;
let hasLoggedFrameWaitingMessage = false;

let inputSocket: Socket | undefined;
let inputReconnectTimer: ReturnType<typeof setTimeout> | undefined;
let inputPipeClientStarted = false;
let hasLoggedInputWaitingMessage = false;

const pressedActions = new Set<string>();

let pendingBytes = Buffer.alloc(0);
let pendingHeader: FrameHeader | undefined;

// Rendering may take longer than the native helper's 15 FPS output. We keep
// only the newest complete frame instead of building an ever-growing queue.
let latestFrame: RawFrame | undefined;
let rendering = false;

function resetParser(): void {
	pendingBytes = Buffer.alloc(0);
	pendingHeader = undefined;
}

function scheduleFrameReconnect(): void {
	if (frameReconnectTimer) {
		return;
	}

	frameReconnectTimer = setTimeout(() => {
		frameReconnectTimer = undefined;
		connectToFramePipe();
	}, RECONNECT_DELAY_MS);
}

function connectToFramePipe(): void {
	if (frameSocket && !frameSocket.destroyed) {
		return;
	}

	resetParser();

	const nextSocket = createConnection(FRAME_PIPE_PATH);
	frameSocket = nextSocket;

	nextSocket.once("connect", () => {
		hasLoggedFrameWaitingMessage = false;
		console.log(
			`Connected to native DoomDeck capture at ${FRAME_PIPE_PATH}.`
		);
	});

	nextSocket.on("data", (chunk: Buffer) => {
		try {
			consumePipeBytes(chunk);
		} catch (error) {
			console.error("Invalid DoomDeck frame stream:", error);
			nextSocket.destroy();
		}
	});

	nextSocket.on("error", (error: NodeJS.ErrnoException) => {
		if (error.code === "ENOENT" || error.code === "ECONNREFUSED") {
			if (!hasLoggedFrameWaitingMessage) {
				console.log(
					"Waiting for DoomDeckCapture.exe to create the frame pipe..."
				);
				hasLoggedFrameWaitingMessage = true;
			}

			return;
		}

		console.error("DoomDeck frame pipe error:", error);
	});

	nextSocket.once("close", () => {
		if (frameSocket === nextSocket) {
			frameSocket = undefined;
		}

		resetParser();
		scheduleFrameReconnect();
	});
}

function startFramePipeClient(): void {
	if (framePipeClientStarted) {
		return;
	}

	framePipeClientStarted = true;
	connectToFramePipe();
}

function scheduleInputReconnect(): void {
	if (inputReconnectTimer) {
		return;
	}

	inputReconnectTimer = setTimeout(() => {
		inputReconnectTimer = undefined;
		connectToInputPipe();
	}, RECONNECT_DELAY_MS);
}

function connectToInputPipe(): void {
	if (inputSocket && !inputSocket.destroyed) {
		return;
	}

	const nextSocket = createConnection(INPUT_PIPE_PATH);
	inputSocket = nextSocket;

	nextSocket.once("connect", () => {
		hasLoggedInputWaitingMessage = false;
		console.log(
			`Connected to native DoomDeck input at ${INPUT_PIPE_PATH}.`
		);
	});

	nextSocket.on("error", (error: NodeJS.ErrnoException) => {
		if (error.code === "ENOENT" || error.code === "ECONNREFUSED") {
			if (!hasLoggedInputWaitingMessage) {
				console.log(
					"Waiting for DoomDeckCapture.exe to create the input pipe..."
				);
				hasLoggedInputWaitingMessage = true;
			}

			return;
		}

		console.error("DoomDeck input pipe error:", error);
	});

	nextSocket.once("close", () => {
		if (inputSocket === nextSocket) {
			inputSocket = undefined;
		}

		/*
		 * Forget the plugin-side pressed state after a disconnect. The native
		 * helper should release every simulated key when its pipe disconnects,
		 * preventing movement or firing from becoming stuck.
		 */
		pressedActions.clear();
		scheduleInputReconnect();
	});
}

function startInputPipeClient(): void {
	if (inputPipeClientStarted) {
		return;
	}

	inputPipeClientStarted = true;
	connectToInputPipe();
}

function getControl(column: number, row: number): DoomControl | undefined {
	return CONTROL_BY_POSITION[row]?.[column];
}

function sendControl(control: DoomControl, action: InputAction): void {
	const currentSocket = inputSocket;

	if (
		!currentSocket ||
		currentSocket.destroyed ||
		!currentSocket.writable
	) {
		return;
	}

	const packet = Buffer.from([action, control]);

	currentSocket.write(packet, (error?: Error | null) => {
		if (error) {
			console.error("Failed to send DoomDeck input packet:", error);
		}
	});
}

function consumePipeBytes(chunk: Buffer): void {
	pendingBytes = Buffer.concat([pendingBytes, chunk]);

	while (true) {
		if (!pendingHeader) {
			if (pendingBytes.length < HEADER_SIZE) {
				return;
			}

			const headerBytes = pendingBytes.subarray(0, HEADER_SIZE);
			pendingBytes = pendingBytes.subarray(HEADER_SIZE);
			pendingHeader = parseHeader(headerBytes);
		}

		if (pendingBytes.length < pendingHeader.payloadSize) {
			return;
		}

		const pixels = Buffer.from(
			pendingBytes.subarray(0, pendingHeader.payloadSize)
		);

		pendingBytes = pendingBytes.subarray(pendingHeader.payloadSize);

		const frame: RawFrame = {
			...pendingHeader,
			pixels
		};

		pendingHeader = undefined;
		queueFrame(frame);
	}
}

function parseHeader(header: Buffer): FrameHeader {
	const magic = header.toString("ascii", 0, 4);
	const version = header.readUInt32LE(4);
	const width = header.readUInt32LE(8);
	const height = header.readUInt32LE(12);
	const channels = header.readUInt32LE(16);
	const payloadSize = header.readUInt32LE(20);
	const frameNumber = header.readBigUInt64LE(24);

	if (magic !== PROTOCOL_MAGIC) {
		throw new Error(`Bad frame magic ${JSON.stringify(magic)}.`);
	}

	if (version !== PROTOCOL_VERSION) {
		throw new Error(`Unsupported frame protocol version ${version}.`);
	}

	if (width <= 0 || height <= 0) {
		throw new Error(`Invalid frame dimensions ${width}x${height}.`);
	}

	if (channels < 1 || channels > 4) {
		throw new Error(`Invalid channel count ${channels}.`);
	}

	const expectedPayloadSize = width * height * channels;

	if (
		payloadSize !== expectedPayloadSize ||
		payloadSize > MAX_FRAME_PAYLOAD
	) {
		throw new Error(
			`Invalid payload size ${payloadSize}; expected ${expectedPayloadSize}.`
		);
	}

	return {
		width,
		height,
		channels: channels as 1 | 2 | 3 | 4,
		payloadSize,
		frameNumber
	};
}

function queueFrame(frame: RawFrame): void {
	latestFrame = frame;

	if (!rendering) {
		void renderLatestFrames();
	}
}

async function renderLatestFrames(): Promise<void> {
	if (rendering) {
		return;
	}

	rendering = true;

	try {
		while (latestFrame) {
			const frame = latestFrame;
			latestFrame = undefined;

			if (visibleKeys.size === 0) {
				continue;
			}

			await renderer.updateRaw(
				frame.pixels,
				frame.width,
				frame.height,
				frame.channels
			);

			await Promise.all(
				Array.from(visibleKeys.values()).map(async (key) => {
					const tile = renderer.getTile(key.column, key.row);
					await key.action.setImage(tile);
				})
			);
		}
	} catch (error) {
		console.error("Failed to render a DoomDeck frame:", error);
	} finally {
		rendering = false;

		// A frame can arrive between the final while-condition check and this
		// finally block. Start another pass if that happened.
		if (latestFrame) {
			void renderLatestFrames();
		}
	}
}

@action({ UUID: "com.winlonghorn.doomdeck.doom" })
export class IncrementCounter extends SingletonAction {
	override async onWillAppear(
		ev: WillAppearEvent
	): Promise<void> {
		if (!("coordinates" in ev.payload)) {
			return;
		}

		const { column, row } = ev.payload.coordinates;

		visibleKeys.set(ev.action.id, {
			action: ev.action,
			column,
			row
		});

		await ev.action.setTitle("");
		startFramePipeClient();
		startInputPipeClient();
	}

	override onWillDisappear(ev: WillDisappearEvent): void {
		/*
		 * Release a control if its Stream Deck action disappears while held,
		 * such as when changing profiles or unloading the plugin.
		 */
		if (pressedActions.delete(ev.action.id)) {
			const visibleKey = visibleKeys.get(ev.action.id);

			if (visibleKey) {
				const control = getControl(
					visibleKey.column,
					visibleKey.row
				);

				if (control !== undefined) {
					sendControl(control, InputAction.Release);
				}
			}
		}

		visibleKeys.delete(ev.action.id);
	}

	override onKeyDown(ev: KeyDownEvent): void {
		if (!("coordinates" in ev.payload)) {
			return;
		}

		if (pressedActions.has(ev.action.id)) {
			return;
		}

		const { column, row } = ev.payload.coordinates;
		const control = getControl(column, row);

		if (control === undefined) {
			return;
		}

		pressedActions.add(ev.action.id);

		sendControl(control, InputAction.Press);
	}

	override onKeyUp(ev: KeyUpEvent): void {
		if (!("coordinates" in ev.payload)) {
			return;
		}

		if (!pressedActions.delete(ev.action.id)) {
			return;
		}

		const { column, row } = ev.payload.coordinates;
		const control = getControl(column, row);

		if (control === undefined) {
			return;
		}

		sendControl(control, InputAction.Release);
	}
}