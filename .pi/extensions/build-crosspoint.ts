/**
 * Build CrossPoint - Builds the CrossPoint e-reader firmware (default environment).
 *
 * This tool is preferred over direct `pio run` for building the project.
 */

import { spawn } from "child_process";
import { Type } from "typebox";
import { defineTool, type ExtensionAPI } from "@earendil-works/pi-coding-agent";

/** Ensure `cargo doc-md -p <crate>` has been run (returns silently on success). */
function build(): Promise<void> {
	return new Promise((resolve, reject) => {
		const child = spawn("pio", ["run", "-e", "default"], { stdio: ["ignore", "ignore", "pipe"] });
		const errChunks: Buffer[] = [];
		child.stderr.on("data", (data) => errChunks.push(data));
		child.on("error", reject);
		child.on("close", (code) => {
			if (code !== 0) {
				reject(new Error(`pio run -e default failed (${code}): ${Buffer.concat(errChunks).toString()}`));
			} else {
				resolve();
			}
		});
	});
}

const buildCrosspointTool = defineTool({
	name: "build-crosspoint",
	label: "Build CrossPoint",
	description:
		"Build the CrossPoint e-reader firmware (default environment). Always preferred over direct pio run for building the project.",
	promptSnippet: "Build the CrossPoint e-reader firmware via pio run -e default",
	promptGuidelines: [
		"Use build-crosspoint to build the CrossPoint project instead of running pio run directly.",
	],
	parameters: Type.Object({}),

	async execute(_toolCallId, _params, _signal, _onUpdate, _ctx) {
		await build();

		return {
			content: [{ type: "text", text: "Build completed successfully." }],
			details: { success: true },
		};
	},
});

export default function (pi: ExtensionAPI) {
	pi.registerTool(buildCrosspointTool);
}
