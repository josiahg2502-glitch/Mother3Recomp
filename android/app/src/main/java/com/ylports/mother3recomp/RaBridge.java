package com.ylports.mother3recomp;

import android.app.Activity;
import android.os.Build;
import android.widget.Toast;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;

/**
 * RetroAchievements plumbing shared by the setup screen (login) and the
 * native achievement runtime (src/retroachievements.cpp), which calls
 * {@link #request} and {@link #toast} over JNI.
 *
 * Only the user name and API token are stored, in app-private
 * retroachievements.ini; the password is sent once to obtain the token.
 */
public final class RaBridge {
    // Must match kClientName / kClientVersion in src/retroachievements.cpp.
    static final String CLIENT_NAME = "Mother3Recomp";
    static final String CLIENT_VERSION = "0.1.3";
    static final String CREDENTIALS_FILE = "retroachievements.ini";

    private static final String DOREQUEST_URL = "https://retroachievements.org/dorequest.php";
    private static final int TIMEOUT_MS = 15000;
    // rcheevos RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR.
    private static final int RETRYABLE_CLIENT_ERROR = -2;

    private RaBridge() {}

    /** Read by native code through JNI field access. */
    public static final class Response {
        public int status;
        public byte[] body;
    }

    /** Blocking HTTP request; called from the native network worker thread. */
    public static Response request(String url, String postData, String contentType,
                                   String userAgent) {
        Response out = new Response();
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(TIMEOUT_MS);
            connection.setReadTimeout(TIMEOUT_MS);
            connection.setRequestProperty("User-Agent", userAgent);
            if (postData != null) {
                byte[] data = postData.getBytes(StandardCharsets.UTF_8);
                connection.setRequestMethod("POST");
                connection.setDoOutput(true);
                connection.setRequestProperty("Content-Type", contentType != null
                    ? contentType : "application/x-www-form-urlencoded");
                connection.setFixedLengthStreamingMode(data.length);
                try (OutputStream output = connection.getOutputStream()) {
                    output.write(data);
                }
            }
            out.status = connection.getResponseCode();
            InputStream input = out.status >= 400
                ? connection.getErrorStream() : connection.getInputStream();
            out.body = input != null ? readAll(input) : new byte[0];
        } catch (IOException error) {
            out.status = RETRYABLE_CLIENT_ERROR;
            out.body = new byte[0];
        } finally {
            if (connection != null) connection.disconnect();
        }
        return out;
    }

    /** Fallback notification when the native overlay has no font stack. */
    public static void toast(Activity activity, byte[] utf8) {
        if (activity == null || utf8 == null) return;
        final String text = new String(utf8, StandardCharsets.UTF_8);
        activity.runOnUiThread(
            () -> Toast.makeText(activity, text, Toast.LENGTH_LONG).show());
    }

    static String userAgent() {
        return CLIENT_NAME + "/" + CLIENT_VERSION + " (Android " + Build.VERSION.RELEASE + ")";
    }

    /** Exchanges a password for an API token. Blocking: call off the UI thread. */
    static String[] login(String user, String password) throws IOException {
        String body = "r=login2&u=" + URLEncoder.encode(user, "UTF-8")
            + "&p=" + URLEncoder.encode(password, "UTF-8");
        Response response = request(DOREQUEST_URL, body,
            "application/x-www-form-urlencoded", userAgent());
        if (response.status == RETRYABLE_CLIENT_ERROR)
            throw new IOException("Network error: check the Wi-Fi connection.");
        try {
            JSONObject json = new JSONObject(new String(response.body, StandardCharsets.UTF_8));
            if (!json.optBoolean("Success", false)) {
                throw new IOException(json.optString("Error",
                    "Login failed (HTTP " + response.status + ")."));
            }
            return new String[] {json.getString("User"), json.getString("Token")};
        } catch (JSONException error) {
            throw new IOException("Unexpected server response (HTTP " + response.status + ").");
        }
    }

    static void saveCredentials(File dir, String user, String token) throws IOException {
        File target = new File(dir, CREDENTIALS_FILE);
        File temp = new File(dir, CREDENTIALS_FILE + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temp)) {
            output.write(("user=" + user + "\ntoken=" + token + "\n")
                .getBytes(StandardCharsets.UTF_8));
        }
        if (!temp.renameTo(target)) {
            temp.delete();
            throw new IOException("Unable to save " + target);
        }
    }

    /** The logged-in user name, or null. */
    static String savedUser(File dir) {
        File file = new File(dir, CREDENTIALS_FILE);
        if (!file.isFile()) return null;
        try (FileInputStream input = new FileInputStream(file)) {
            for (String line : new String(readAll(input), StandardCharsets.UTF_8).split("\n")) {
                if (line.startsWith("user=")) return line.substring(5).trim();
            }
        } catch (IOException ignored) {
            // Treated as logged out.
        }
        return null;
    }

    static void logout(File dir) {
        new File(dir, CREDENTIALS_FILE).delete();
    }

    private static byte[] readAll(InputStream input) throws IOException {
        try (InputStream in = input) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16 * 1024];
            int read;
            while ((read = in.read(buffer)) >= 0) out.write(buffer, 0, read);
            return out.toByteArray();
        }
    }
}
