require("dotenv").config();
const http = require("http");
const { URL } = require("url");

const PORT = Number(process.env.PORT || 3000);
const SUPABASE_URL = process.env.SUPABASE_URL || "";
const SUPABASE_KEY = process.env.SUPABASE_KEY || "";
const TABLE = "lab1_sensor_data";

let latestLatency = null;
const sseClients = new Set();

function sendJson(res, status, data) {
  res.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
    "Access-Control-Allow-Origin": "*"
  });
  res.end(JSON.stringify(data));
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let raw = "";
    req.on("data", chunk => raw += chunk);
    req.on("end", () => {
      try {
        resolve(JSON.parse(raw || "{}"));
      } catch {
        reject(new Error("Invalid JSON"));
      }
    });
    req.on("error", reject);
  });
}

async function supabaseGet(query) {
  if (!SUPABASE_URL || !SUPABASE_KEY) {
    throw new Error("SUPABASE_URL / SUPABASE_KEY is not configured.");
  }

  const response = await fetch(
    SUPABASE_URL + query,
    {
      headers: {
        apikey: SUPABASE_KEY,
        Authorization: "Bearer " + SUPABASE_KEY
      }
    }
  );

  const text = await response.text();

  if (!response.ok) {
    throw new Error("Supabase error: " + text);
  }

  return text ? JSON.parse(text) : [];
}

function broadcast(payload) {
  const message = `data: ${JSON.stringify(payload)}\n\n`;

  for (const client of sseClients) {
    try {
      client.write(message);
    } catch {
      sseClients.delete(client);
    }
  }
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);

    // Health check
    if (req.method === "GET" && url.pathname === "/api/health") {
      return sendJson(res, 200, {
        ok: true,
        service: "IoT Lab 1 backend"
      });
    }

    // React frontend calls this endpoint.
    if (req.method === "GET" && url.pathname === "/api/latest") {
      const rows = await supabaseGet(
        `/rest/v1/${TABLE}` +
        `?select=created_at,device_id,temperature,humidity` +
        `&order=created_at.desc&limit=1`
      );

      return sendJson(res, 200, rows[0] || null);
    }

    if (req.method === "GET" && url.pathname === "/api/history") {
      let query =
        `/rest/v1/${TABLE}` +
        `?select=created_at,device_id,temperature,humidity` +
        `&order=created_at.desc&limit=500`;

      const from = url.searchParams.get("from");
      const to = url.searchParams.get("to");

      if (from) {
        query += `&created_at=gte.${encodeURIComponent(from + "T00:00:00Z")}`;
      }

      if (to) {
        query += `&created_at=lte.${encodeURIComponent(to + "T23:59:59Z")}`;
      }

      const rows = await supabaseGet(query);
      return sendJson(res, 200, rows);
    }

    // Last 5 five-minute buckets.
    if (req.method === "GET" && url.pathname === "/api/chart") {
      const rows = await supabaseGet(
        `/rest/v1/${TABLE}` +
        `?select=created_at,temperature,humidity` +
        `&order=created_at.desc&limit=500`
      );

      const buckets = new Map();
      const bucketMs = 5 * 60 * 1000;

      for (const row of rows) {
        const time = new Date(row.created_at).getTime();
        const bucketTime =
          Math.floor(time / bucketMs) * bucketMs;

        if (!buckets.has(bucketTime)) {
          buckets.set(bucketTime, {
            time: bucketTime,
            sumT: 0,
            sumH: 0,
            count: 0
          });
        }

        const bucket = buckets.get(bucketTime);
        bucket.sumT += Number(row.temperature);
        bucket.sumH += Number(row.humidity);
        bucket.count++;
      }

      const result = [...buckets.values()]
        .sort((a, b) => a.time - b.time)
        .slice(-5)
        .map(bucket => ({
          time: new Date(bucket.time).toISOString(),
          temperature: Number(
            (bucket.sumT / bucket.count).toFixed(1)
          ),
          humidity: Number(
            (bucket.sumH / bucket.count).toFixed(1)
          )
        }));

      return sendJson(res, 200, result);
    }

    // Browser receives t7 by SSE.
    if (req.method === "GET" && url.pathname === "/events") {
      res.writeHead(200, {
        "Content-Type": "text/event-stream; charset=utf-8",
        "Cache-Control": "no-cache",
        Connection: "keep-alive",
        "Access-Control-Allow-Origin": "*"
      });

      res.write(": connected\n\n");
      sseClients.add(res);

      req.on("close", () => {
        sseClients.delete(res);
      });

      return;
    }

    if (req.method === "GET" && url.pathname === "/api/latency") {
      return sendJson(res, 200, latestLatency || null);
    }

    // ESP8266 -> backend.
    // t4 = backend receives request.
    // t5 = cloud processing complete.
    // t6 = ALERT_PUBLISHED.
    if (req.method === "POST" && url.pathname === "/api/latency") {
      const t4 = Date.now();
      const body = await readBody(req);

      if (
        body.t0 === undefined ||
        body.t1 === undefined ||
        body.t2 === undefined ||
        body.temperature === undefined ||
        body.humidity === undefined
      ) {
        return sendJson(res, 400, {
          error: "Missing t0/t1/t2/temperature/humidity"
        });
      }

      const fireSuspected =
        Boolean(body.fire_suspected) ||
        Number(body.temperature) >= 30;

      const event = {
        event_id: String(
          body.event_id || `ESP8266_LAB1_${body.t2}`
        ),
        device_id: String(
          body.device_id || "ESP8266_LAB1"
        ),
        temperature: Number(body.temperature),
        humidity: Number(body.humidity),
        fire_suspected: fireSuspected,
        t0: Number(body.t0),
        t1: Number(body.t1),
        t2: Number(body.t2),
        t4
      };

      // t5 = processing complete.
      event.t5 = Date.now();

      if (fireSuspected) {
        // t6 = cloud marks ALERT_PUBLISHED.
        event.t6 = Date.now();

        latestLatency = event;

        broadcast({
          type: "alert",
          latency: latestLatency
        });
      }

      return sendJson(res, 200, {
        ok: true,
        t4: event.t4,
        t5: event.t5,
        t6: event.t6 || null
      });
    }

    // ESP8266 sends t3 after its first request has finished.
    // t3 is logged for the timeline but is not used to invent
    // any new latency formula.
    if (
      req.method === "POST" &&
      url.pathname === "/api/latency/complete"
    ) {
      const body = await readBody(req);

      if (
        !body.event_id ||
        body.t3 === undefined
      ) {
        return sendJson(res, 400, {
          error: "Missing event_id/t3"
        });
      }

      if (
        latestLatency &&
        latestLatency.event_id === String(body.event_id)
      ) {
        latestLatency.t3 = Number(body.t3);

        broadcast({
          type: "t3_complete",
          latency: latestLatency
        });
      }

      return sendJson(res, 200, { ok: true });
    }

    sendJson(res, 404, { error: "Not found" });
  } catch (error) {
    console.error(error);
    sendJson(res, 500, { error: error.message });
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`Backend: http://localhost:${PORT}`);
});
