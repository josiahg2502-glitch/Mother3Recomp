package com.ylports.mother3recomp.cloud;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/** Minimal blocking HTTP client for the cloud save providers. Call off the UI thread. */
final class CloudHttp {
    static final String USER_AGENT = "Mother3Recomp/0.1.3 (Android)";
    private static final int TIMEOUT_MS = 20000;

    private CloudHttp() {}

    static final class Response {
        final int status;
        final byte[] body;
        final Map<String, List<String>> headers;

        Response(int status, byte[] body, Map<String, List<String>> headers) {
            this.status = status;
            this.body = body;
            this.headers = headers;
        }

        boolean ok() {
            return status >= 200 && status < 300;
        }

        String text() {
            return new String(body, StandardCharsets.UTF_8);
        }

        JSONObject json() throws IOException {
            try {
                return new JSONObject(text());
            } catch (JSONException error) {
                throw new IOException("Unexpected server response (HTTP " + status + ")");
            }
        }

        JSONArray jsonArray() throws IOException {
            try {
                return new JSONArray(text());
            } catch (JSONException error) {
                throw new IOException("Unexpected server response (HTTP " + status + ")");
            }
        }

        /** The server's own error text (RomM "detail", Hoard "error"), or the status. */
        String errorMessage() {
            try {
                JSONObject json = new JSONObject(text());
                Object detail = json.opt("detail");
                if (detail instanceof String) return (String) detail;
                String error = json.optString("error", "");
                if (!error.isEmpty()) return error;
            } catch (JSONException ignored) {
                // Not JSON: fall through to the status line.
            }
            return "HTTP " + status;
        }

        /** Throws with the server's message unless the status is 2xx. */
        Response check(String action) throws IOException {
            if (!ok()) throw new IOException(action + " failed: " + errorMessage());
            return this;
        }

        String header(String name) {
            for (Map.Entry<String, List<String>> entry : headers.entrySet()) {
                if (entry.getKey() != null && entry.getKey().equalsIgnoreCase(name)
                        && !entry.getValue().isEmpty()) {
                    return String.join("\n", entry.getValue());
                }
            }
            return null;
        }
    }

    static Response get(String url, String bearer) throws IOException {
        return send("GET", url, bearer, null, null);
    }

    static Response postJson(String url, String bearer, JSONObject body) throws IOException {
        return send("POST", url, bearer, "application/json",
            body.toString().getBytes(StandardCharsets.UTF_8));
    }

    static Response send(String method, String url, String bearer, String contentType,
                         byte[] body) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        try {
            connection.setConnectTimeout(TIMEOUT_MS);
            connection.setReadTimeout(TIMEOUT_MS);
            connection.setInstanceFollowRedirects(false);
            connection.setRequestMethod(method);
            connection.setRequestProperty("User-Agent", USER_AGENT);
            connection.setRequestProperty("Accept", "application/json, */*");
            if (bearer != null) connection.setRequestProperty("Authorization", "Bearer " + bearer);
            if (body != null) {
                connection.setDoOutput(true);
                connection.setRequestProperty("Content-Type", contentType);
                connection.setFixedLengthStreamingMode(body.length);
                try (OutputStream output = connection.getOutputStream()) {
                    output.write(body);
                }
            }
            int status = connection.getResponseCode();
            String location = connection.getHeaderField("Location");
            if (status >= 300 && status < 400 && location != null) {
                // Not followed: the token must not travel to another address
                // (or downgrade to http) behind the player's back.
                URL target = new URL(new URL(url), location);
                throw new IOException("The server redirects to " + target.getProtocol() + "://"
                    + target.getAuthority() + "; use that address in the cloud save settings.");
            }
            InputStream input = status >= 400 ? connection.getErrorStream()
                                              : connection.getInputStream();
            byte[] data = input != null ? readAll(input) : new byte[0];
            return new Response(status, data, connection.getHeaderFields());
        } finally {
            connection.disconnect();
        }
    }

    /** One part of a multipart/form-data body; fileName != null makes it a file. */
    static final class Part {
        final String name;
        final String fileName;
        final byte[] data;

        private Part(String name, String fileName, byte[] data) {
            this.name = name;
            this.fileName = fileName;
            this.data = data;
        }

        static Part field(String name, String value) {
            return new Part(name, null, value.getBytes(StandardCharsets.UTF_8));
        }

        static Part file(String name, String fileName, byte[] data) {
            return new Part(name, fileName, data);
        }
    }

    static Response multipart(String url, String bearer, List<Part> parts) throws IOException {
        String boundary = "m3r-" + UUID.randomUUID();
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (Part part : parts) {
            StringBuilder head = new StringBuilder();
            head.append("--").append(boundary).append("\r\n");
            head.append("Content-Disposition: form-data; name=\"").append(part.name).append('"');
            if (part.fileName != null) {
                head.append("; filename=\"").append(part.fileName).append('"');
                head.append("\r\nContent-Type: application/octet-stream");
            }
            head.append("\r\n\r\n");
            out.write(head.toString().getBytes(StandardCharsets.UTF_8));
            out.write(part.data);
            out.write("\r\n".getBytes(StandardCharsets.UTF_8));
        }
        out.write(("--" + boundary + "--\r\n").getBytes(StandardCharsets.UTF_8));
        return send("POST", url, bearer, "multipart/form-data; boundary=" + boundary,
            out.toByteArray());
    }

    /** Trims a pasted server address to "scheme://host[:port][/path]" without a trailing slash. */
    static String normalizeBaseUrl(String raw) throws IOException {
        String url = raw.trim();
        if (url.isEmpty()) throw new IOException("Enter the server address.");
        if (!url.startsWith("http://") && !url.startsWith("https://")) url = "http://" + url;
        while (url.endsWith("/")) url = url.substring(0, url.length() - 1);
        try {
            new URL(url);
        } catch (IOException error) {
            throw new IOException("Invalid server address: " + raw);
        }
        return url;
    }

    static byte[] readAll(InputStream input) throws IOException {
        try (InputStream in = input) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16 * 1024];
            int read;
            while ((read = in.read(buffer)) >= 0) out.write(buffer, 0, read);
            return out.toByteArray();
        }
    }
}
