import sharp, { type Sharp } from "sharp";

const TILE_SIZE = 144;
const COLUMNS = 5;
const ROWS = 3;

type RawChannels = 1 | 2 | 3 | 4;

export class FrameRenderer {
	private readonly width = TILE_SIZE * COLUMNS;
	private readonly height = TILE_SIZE * ROWS;

	private tiles: string[][] = [];

	async load(imagePath: string): Promise<void> {
		const image = sharp(imagePath).resize(this.width, this.height, {
			fit: "fill"
		});

		await this.buildTiles(image);
	}

	/**
	 * Keeps support for encoded image buffers, although DoomDeck now normally
	 * calls updateRaw() with pixels from the native capture helper.
	 */
	async update(image: Buffer): Promise<void> {
		const resized = sharp(image).resize(this.width, this.height, {
			fit: "fill"
		});

		await this.buildTiles(resized);
	}

	/**
	 * Builds the Stream Deck tiles directly from packed raw pixels.
	 */
	async updateRaw(
		pixels: Buffer,
		width: number,
		height: number,
		channels: RawChannels
	): Promise<void> {
		const expectedBytes = width * height * channels;

		if (pixels.length !== expectedBytes) {
			throw new Error(
				`Raw frame has ${pixels.length} bytes; expected ${expectedBytes}.`
			);
		}

		let image = sharp(pixels, {
			raw: {
				width,
				height,
				channels
			}
		});

		if (width !== this.width || height !== this.height) {
			image = image.resize(this.width, this.height, {
				fit: "fill"
			});
		}

		await this.buildTiles(image);
	}

	private async buildTiles(image: Sharp): Promise<void> {
		const jobs: Promise<{
			column: number;
			row: number;
			dataUrl: string;
		}>[] = [];

		for (let row = 0; row < ROWS; row++) {
			for (let column = 0; column < COLUMNS; column++) {
				jobs.push(
					image
						.clone()
						.extract({
							left: column * TILE_SIZE,
							top: row * TILE_SIZE,
							width: TILE_SIZE,
							height: TILE_SIZE
						})
						.jpeg({
							quality: 75,
							chromaSubsampling: "4:2:0"
						})
						.toBuffer()
						.then((tile) => ({
							column,
							row,
							dataUrl: `data:image/jpeg;base64,${tile.toString("base64")}`
						}))
				);
			}
		}

		const completedTiles = await Promise.all(jobs);

		const nextTiles = Array.from(
			{ length: ROWS },
			() => Array<string>(COLUMNS)
		);

		for (const tile of completedTiles) {
			nextTiles[tile.row][tile.column] = tile.dataUrl;
		}

		// Swap the complete frame in atomically so getTile() never observes a
		// half-built set of tiles.
		this.tiles = nextTiles;
	}

	getTile(column: number, row: number): string {
		const tile = this.tiles[row]?.[column];

		if (!tile) {
			throw new Error(
				`Tile is unavailable at column ${column}, row ${row}.`
			);
		}

		return tile;
	}
}