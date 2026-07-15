// ============================================================================
//  OfficeAir - Uptime Dashboard generator
//
//  Builds a date x hour matrix on the "Uptime Dashboard" sheet from the raw
//  measurement log: each cell holds the percentage of expected measurements
//  that actually arrived in that hour (10-minute cadence -> 6 pings/hour).
//
//  Pair it with a conditional-formatting color scale on the dashboard sheet
//  (recommended: Minpoint = Number 0, Midpoint = Number 90, Maxpoint =
//  Number 100) - empty cells (future hours) are skipped by the color scale
//  automatically, so the "future" never shows up as false red downtime.
//
//  Run it manually or from a time-driven trigger (e.g. every 10 minutes).
// ============================================================================

function generateUptimeDashboard() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();

  // 1. Exact sheet name ("Raw")
  var rawSheet = ss.getSheetByName("Raw");

  // 2. Safety check
  if (!rawSheet) {
    Logger.log("Error: no sheet named 'Raw' found! Dashboard update aborted.");
    return;
  }

  // 3. Prepare the dashboard sheet
  var dashSheet = ss.getSheetByName("Uptime Dashboard");
  if (!dashSheet) {
    dashSheet = ss.insertSheet("Uptime Dashboard");
  } else {
    dashSheet.clearContents(); // contents only - formatting (colors) survives
  }

  // 4. Read the data (col A: timestamp, col B: temperature)
  var lr = rawSheet.getLastRow();
  if (lr < 2) {
    Logger.log("The 'Raw' sheet has no measurements yet.");
    return;
  }

  var rawData = rawSheet.getRange(2, 1, lr - 1, 2).getValues();

  // SETTINGS
  var EXPECTED_PINGS_PER_HOUR = 6;  // 10-minute cadence -> 6 measurements/hour
  var HIDE_CURRENT_HOUR = true;     // true: the in-progress (not yet closed) hour stays empty
                                    // false: the in-progress hour shows a partial %

  var tz = ss.getSpreadsheetTimeZone();
  var dataMap = {};

  // 5. Data processing with jitter protection
  for (var i = 0; i < rawData.length; i++) {
    var timestamp = new Date(rawData[i][0]);
    var tempValue = rawData[i][1];

    if (isNaN(timestamp.getTime()) || tempValue === "") continue;

    // Jitter protection: round to the nearest 10-minute boundary
    var tenMinutesInMs = 1000 * 60 * 10;
    var roundedTimestamp = new Date(Math.round(timestamp.getTime() / tenMinutesInMs) * tenMinutesInMs);

    // Timezone-correct date string and hour
    // (both derived from the SPREADSHEET timezone - getHours() would use the
    // script's timezone, which may differ and misfile measurements around midnight)
    var dateStr = Utilities.formatDate(roundedTimestamp, tz, "yyyy-MM-dd");
    var hour = Number(Utilities.formatDate(roundedTimestamp, tz, "H"));

    if (!dataMap[dateStr]) {
      dataMap[dateStr] = {};
      for (var h = 0; h < 24; h++) dataMap[dateStr][h] = 0;
    }
    dataMap[dateStr][hour]++;
  }

  // DETERMINE "NOW" IN THE SPREADSHEET TIMEZONE
  // Used per cell to decide whether it lies in the future (-> empty cell,
  // which the conditional-formatting color scale skips automatically)
  var now = new Date();
  var todayStr = Utilities.formatDate(now, tz, "yyyy-MM-dd");
  var currentHour = Number(Utilities.formatDate(now, tz, "H"));

  // 6. Build the output matrix
  var dates = Object.keys(dataMap).sort().reverse(); // most recent day on top
  var output = [];

  var header = ["Date / Hour"];
  for (var h = 0; h < 24; h++) {
    header.push(h + ":00");
  }
  output.push(header);

  for (var d = 0; d < dates.length; d++) {
    var dStr = dates[d];
    var row = [dStr];
    for (var h = 0; h < 24; h++) {

      // FUTURE CURTAIN
      // With yyyy-MM-dd format, string comparison is chronologically correct.
      var isFutureDay   = dStr > todayStr;
      var isFutureHour  = (dStr === todayStr) && (h > currentHour);
      var isCurrentHour = (dStr === todayStr) && (h === currentHour);

      if (isFutureDay || isFutureHour || (HIDE_CURRENT_HOUR && isCurrentHour)) {
        row.push(""); // empty cell: the color scale won't paint it - no false red
        continue;
      }

      var actualPings = dataMap[dStr][h];
      var uptimePercentage = (actualPings / EXPECTED_PINGS_PER_HOUR) * 100;

      // Clip any extra measurement that slipped through due to jitter
      if (uptimePercentage > 100) uptimePercentage = 100;

      row.push(uptimePercentage);
    }
    output.push(row);
  }

  // 7. Write data and format the sheet
  dashSheet.getRange(1, 1, output.length, output[0].length).setValues(output);

  dashSheet.setFrozenRows(1);
  dashSheet.setFrozenColumns(1);

  dashSheet.setColumnWidth(1, 100);
  for (var col = 2; col <= 25; col++) {
    dashSheet.setColumnWidth(col, 35);
  }

  Logger.log("Dashboard updated: jitter handled, future hours hidden.");
}
