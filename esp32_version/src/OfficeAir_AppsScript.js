/** @OnlyCurrentDoc */

// ============================================================================
//  OfficeAir - Google Apps Script backend
//
//  Receives measurements from the ESP32 via GET parameters and appends them
//  to the "Raw" sheet, matching columns by header name.
//
//  Key design points:
//   - IDEMPOTENT: re-sent measurements (lost ACK) are answered with "OK"
//     without writing, so the device's retry logic can never fight the
//     flood protection.
//   - Header-driven: add a new column to the sheet + a new URL parameter
//     on the device, and it just works - no code change needed.
// ============================================================================

var SHEET_NAME = "Raw";

function doGet(e) {
  // Script lock: guards against concurrent runs of this script only
  var lock = LockService.getScriptLock();
  lock.waitLock(30000);

  try {
    var doc = SpreadsheetApp.getActiveSpreadsheet();
    var sheet = doc.getSheetByName(SHEET_NAME);
    var headers = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getValues()[0];

    // ==========================================
    // 1. INCOMING TIMESTAMP
    // The ESP always sends a Unix epoch; server time is the fallback.
    // ==========================================
    var epoch = e.parameter.time ? Number(e.parameter.time)
                                 : Math.floor(Date.now() / 1000);
    var incomingTime = new Date(epoch * 1000);

    // ==========================================
    // 2. TIMESTAMP OF THE LAST SAVED MEASUREMENT
    // Primarily from the fast ScriptProperties store; if empty (first run
    // with this version), bootstrap it from the last row of the sheet.
    // ==========================================
    var props = PropertiesService.getScriptProperties();
    var lastEpoch = Number(props.getProperty("lastEpoch") || 0);

    if (!lastEpoch) {
      var lastRow = sheet.getLastRow();
      var timeColIndex = headers.indexOf("time") + 1;
      if (lastRow >= 2 && timeColIndex > 0) {
        var v = sheet.getRange(lastRow, timeColIndex).getValue();
        if (v instanceof Date) {
          lastEpoch = Math.floor(v.getTime() / 1000);
        }
      }
    }

    // ==========================================
    // 3. DEDUP - THE KEY TO IDEMPOTENCE
    // The ESP sends strictly increasing timestamps (FIFO buffer).
    // If an incoming time is <= the last saved one, it is guaranteed to be
    // a re-send of an already-saved measurement (lost ACK). We answer "OK"
    // WITHOUT writing, so the ESP can safely pop it from its buffer and
    // move on. This eliminates the infinite retry <-> flood-protection
    // conflict.
    // ==========================================
    if (epoch > 0 && epoch <= lastEpoch) {
      return ContentService.createTextOutput("OK");
    }

    // ==========================================
    // 4. FLOOD PROTECTION
    // Only runs for GENUINELY new, never-seen data. The ESP's measurement
    // cycle is ~30 s minimum, so a new timestamp within 10 s cannot come
    // from it - this purely guards against external spam / broken clients.
    // ==========================================
    if (lastEpoch > 0 && (epoch - lastEpoch) < 10) {
      return ContentService.createTextOutput("Error: Flood protection active. Request ignored.");
    }

    // ==========================================
    // 5. BUILD THE ROW FROM THE HEADER
    // trim() guards against stray whitespace left in header cells.
    // ==========================================
    var row = headers.map(function (h) {
      var name = String(h).trim();
      if (name === "time") return incomingTime;
      var val = e.parameter[name];
      return val !== undefined ? val : "";
    });

    sheet.appendRow(row);

    // Only update after a SUCCESSFUL write - if appendRow throws,
    // lastEpoch stays put and the retry can try again.
    props.setProperty("lastEpoch", String(epoch));

    return ContentService.createTextOutput("OK");

  } catch (err) {
    return ContentService.createTextOutput("Error:" + JSON.stringify(err));
  } finally {
    lock.releaseLock();
  }
}
