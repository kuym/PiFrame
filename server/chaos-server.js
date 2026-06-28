////////////////////////////////////////////////////////////////
//
// PiFrame chaos server
//
// (c) Kuy Mainwaring. All rights reserved.
//
//   A deliberately misbehaving variant of server.js used to
// exercise a PiFrame client's error handling and (for the
// framebuffer edition) its diagnostic HUD.  Every request to the
// photo endpoint is answered with a randomly chosen failure mode:
// HTTP errors, dropped/half-open connections, malformed image
// data served with image/* MIME types, mismatched or bogus MIME
// types, truncated bodies, lied-about Content-Length, hangs,
// trickled responses, raw non-HTTP garbage, and so on.  A small
// fraction of requests return a genuine, valid image so the
// client periodically recovers (and the HUD clears).
//
//   It is built on Node's raw http/net APIs (no "dagger"
// dependency) so it can manipulate the socket directly.
//
// Usage:
//   node server/chaos-server.js [port]      (default port 3000)
//
//   Point a client at it, e.g.:
//     ./piframe-fb "http://10.0.0.84:3000/v1/nextPhoto"
//
//   An always-OK sanity endpoint is available at /health.
//
////////////////////////////////////////////////////////////////

"use strict";

const http   = require("http");
const fs     = require("fs");
const path   = require("path");
const crypto = require("crypto");

const PORT = parseInt(process.argv[2], 10) || 3000;


////////////////////////////////////////////////////////////////
// Sample assets: the project's test JPEGs (used both as valid
//   responses and as raw material to corrupt), plus one tiny but
//   valid PNG so we can occasionally return a good PNG too.

const sampleJPEGs = ["test1.jpg", "test2.jpg", "test3.jpg", "test4.jpg"]
	.map(name => path.join(__dirname, name))
	.filter(p => fs.existsSync(p))
	.map(p => fs.readFileSync(p));

if(sampleJPEGs.length === 0)
{
	// minimal synthetic JPEG so the server still runs without the test files
	sampleJPEGs.push(Buffer.from([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0xFF, 0xD9]));
}

// a valid 1x1 PNG
const validPNG = Buffer.from(
	"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==",
	"base64");

// magic-byte prefixes for crafting "valid header, garbage body" images
const JPEG_MAGIC = Buffer.from([0xFF, 0xD8, 0xFF]);
const PNG_MAGIC  = Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);


////////////////////////////////////////////////////////////////
// Small random helpers.

function randInt(n)            { return Math.floor(Math.random() * n); }
function choice(array)         { return array[randInt(array.length)]; }
function randomJPEG()          { return choice(sampleJPEGs); }
function garbage(min, max)     { return crypto.randomBytes(min + randInt(Math.max(1, max - min))); }

// flip ~5% of the bytes of a copy of buf
function corrupt(buf)
{
	const out = Buffer.from(buf);
	const flips = Math.max(1, Math.floor(out.length * 0.05));
	for(let i = 0; i < flips; i++)
		out[randInt(out.length)] = randInt(256);
	return out;
}

// keep a random leading portion of buf
function truncate(buf)
{
	const keep = 1 + randInt(Math.max(1, buf.length - 1));
	return buf.slice(0, keep);
}

// concatenate a magic-byte header with random garbage
function magicPlusGarbage(magic)
{
	return Buffer.concat([magic, garbage(256, 4096)]);
}


////////////////////////////////////////////////////////////////
// Response helpers.

// Send a complete, well-formed HTTP response with a fixed body.
function send(res, status, contentType, body, extraHeaders)
{
	const headers = Object.assign({ "Content-Type": contentType }, extraHeaders || {});
	if(Buffer.isBuffer(body))
		headers["Content-Length"] = body.length;

	res.writeHead(status, headers);
	res.end(body);
}

// Grab the raw socket so we can do things the http layer won't allow
//   (incomplete headers, lied-about Content-Length, raw garbage, etc).
function rawSocket(res)
{
	const socket = res.socket;
	// stop the http layer from also trying to write/timeout this response
	if(socket)
		socket.on("error", () => {});	// swallow ECONNRESET etc.
	return socket;
}


////////////////////////////////////////////////////////////////
// Failure modes.  Each entry has a weight (relative likelihood)
//   and a handler(req, res).  Weights are tuned so the client sees
//   mostly errors but recovers to a real image now and then.

const SCENARIOS =
[
	//// --- the occasional success (client recovers, HUD clears) ---

	{ name: "valid JPEG", weight: 7, fn: (req, res) =>
		send(res, 200, "image/jpeg", randomJPEG()) },

	{ name: "valid PNG", weight: 4, fn: (req, res) =>
		send(res, 200, "image/png", validPNG) },


	//// --- HTTP status errors (FAILONERROR -> client sees them) ---

	{ name: "HTTP error status", weight: 11, fn: (req, res) =>
	{
		const code = choice([400, 401, 403, 404, 408, 410, 418, 429, 500, 502, 503, 504]);
		send(res, code, "text/plain", Buffer.from(code + " " + (http.STATUS_CODES[code] || "Error")));
	}},

	{ name: "redirect to non-image", weight: 3, fn: (req, res) =>
		send(res, choice([301, 302, 307]), "text/plain", Buffer.from("moved"), { "Location": "/health" }) },


	//// --- dropped / half-open connections ---

	{ name: "drop before responding", weight: 5, fn: (req, res) =>
	{
		const s = rawSocket(res);
		if(s) s.destroy();
	}},

	{ name: "drop after partial headers", weight: 4, fn: (req, res) =>
	{
		const s = rawSocket(res);
		if(!s) return;
		// note: no terminating blank line - headers never complete
		s.write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n");
		s.destroy();
	}},

	{ name: "drop during body", weight: 5, fn: (req, res) =>
	{
		// well-formed headers (chunked), then a partial body and a hard close
		res.writeHead(200, { "Content-Type": "image/jpeg" });
		res.write(truncate(randomJPEG()));
		const s = rawSocket(res);
		if(s) s.destroy();
	}},


	//// --- malformed image data with an image/* MIME type ---

	{ name: "garbage as image/jpeg", weight: 6, fn: (req, res) =>
		send(res, 200, "image/jpeg", garbage(512, 8192)) },

	{ name: "garbage as image/png", weight: 5, fn: (req, res) =>
		send(res, 200, "image/png", garbage(512, 8192)) },

	{ name: "JPEG magic + garbage", weight: 5, fn: (req, res) =>
		send(res, 200, "image/jpeg", magicPlusGarbage(JPEG_MAGIC)) },

	{ name: "PNG magic + garbage", weight: 5, fn: (req, res) =>
		send(res, 200, "image/png", magicPlusGarbage(PNG_MAGIC)) },

	{ name: "corrupted JPEG", weight: 5, fn: (req, res) =>
		send(res, 200, "image/jpeg", corrupt(randomJPEG())) },

	{ name: "truncated JPEG (valid length)", weight: 6, fn: (req, res) =>
		send(res, 200, "image/jpeg", truncate(randomJPEG())) },

	{ name: "empty body as image/jpeg", weight: 4, fn: (req, res) =>
		send(res, 200, "image/jpeg", Buffer.alloc(0)) },


	//// --- mismatched / bogus MIME types ---

	{ name: "real JPEG, wrong MIME", weight: 4, fn: (req, res) =>
		// the framebuffer client sniffs magic bytes, so this should still
		//   decode - it verifies the client is MIME-agnostic
		send(res, 200, choice(["image/png", "text/html", "application/json"]), randomJPEG()) },

	{ name: "HTML body as image/jpeg", weight: 5, fn: (req, res) =>
		send(res, 200, "image/jpeg", Buffer.from("<!DOCTYPE html><html><body><h1>Not an image</h1></body></html>")) },

	{ name: "JSON body as image/png", weight: 4, fn: (req, res) =>
		send(res, 200, "image/png", Buffer.from(JSON.stringify({ error: "this is not a png", ts: Date.now() }))) },

	{ name: "bogus MIME type", weight: 3, fn: (req, res) =>
		send(res, 200, choice(["not/a-real-type", "image/jpeg; charset=utf-8; bogus", "???"]), randomJPEG()) },


	//// --- protocol-level weirdness (raw socket) ---

	{ name: "Content-Length too long (truncated body)", weight: 5, fn: (req, res) =>
	{
		const s = rawSocket(res);
		if(!s) return;
		const body = truncate(randomJPEG());
		s.write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: 99999999\r\n\r\n");
		s.write(body);
		s.destroy();	// never deliver the promised bytes
	}},

	{ name: "raw non-HTTP garbage", weight: 4, fn: (req, res) =>
	{
		const s = rawSocket(res);
		if(!s) return;
		s.write("\x00\x01\x02 GARBAGE-NOT-HTTP \xff\xfe\r\n\r\n");
		s.write(garbage(64, 512));
		s.destroy();
	}},

	{ name: "bad status line", weight: 3, fn: (req, res) =>
	{
		const s = rawSocket(res);
		if(!s) return;
		s.write("HTTP/1.1 999 Totally Fine\r\nContent-Type: image/jpeg\r\nContent-Length: 3\r\n\r\nabc");
		s.end();
	}},


	//// --- timeouts / slowness ---

	{ name: "hang (no response)", weight: 4, fn: (req, res) =>
	{
		// hold the connection open and never answer; a safety timer
		//   eventually drops it so we don't leak sockets forever
		const s = rawSocket(res);
		if(!s) return;
		setTimeout(() => { try { s.destroy(); } catch(e) {} }, 65000);
	}},

	{ name: "slow trickle (never finishes)", weight: 4, fn: (req, res) =>
	{
		res.writeHead(200, { "Content-Type": "image/jpeg" });
		const data = randomJPEG();
		let offset = 0;
		const timer = setInterval(() =>
		{
			if(res.socket && !res.socket.destroyed && offset < data.length)
			{
				res.write(data.slice(offset, offset + 16));	// dribble 16 bytes
				offset += 16;
			}
			else
			{
				clearInterval(timer);
				try { if(res.socket) res.socket.destroy(); } catch(e) {}
			}
		}, 1000);
	}},
];

const TOTAL_WEIGHT = SCENARIOS.reduce((sum, s) => sum + s.weight, 0);

function pickScenario()
{
	let r = Math.random() * TOTAL_WEIGHT;
	for(const s of SCENARIOS)
		if((r -= s.weight) < 0)
			return s;
	return SCENARIOS[SCENARIOS.length - 1];
}


////////////////////////////////////////////////////////////////
// Server.

const server = http.createServer((req, res) =>
{
	const when = new Date().toISOString();
	const from = req.socket.remoteAddress;

	// a stable, always-OK endpoint for sanity checks
	if(req.url.split("?")[0] === "/health")
	{
		console.log(`${when}  ${from}  ${req.url}  -> health OK`);
		send(res, 200, "text/plain", Buffer.from("OK"));
		return;
	}

	const scenario = pickScenario();
	console.log(`${when}  ${from}  ${req.url}  -> ${scenario.name}`);

	try
	{
		scenario.fn(req, res);
	}
	catch(e)
	{
		// a scenario blew up (e.g. socket already gone) - just drop it
		console.error(`  ! scenario "${scenario.name}" threw: ${e.message}`);
		try { if(res.socket) res.socket.destroy(); } catch(_) {}
	}
});

// don't let Node time out the deliberately-slow / hanging responses
server.timeout = 0;
server.keepAliveTimeout = 0;
server.headersTimeout = 0;
server.requestTimeout = 0;

// ignore connection-level errors caused by our own rude socket handling
server.on("clientError", (err, socket) => { try { socket.destroy(); } catch(e) {} });

server.listen(PORT, () =>
{
	console.log(`PiFrame chaos server listening on port ${PORT}`);
	console.log(`  ${sampleJPEGs.length} sample JPEG(s) loaded, ${SCENARIOS.length} failure modes armed`);
	console.log(`  photo endpoint: http://localhost:${PORT}/v1/nextPhoto`);
	console.log(`  sanity endpoint: http://localhost:${PORT}/health`);
});
