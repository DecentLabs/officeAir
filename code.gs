/** @OnlyCurrentDoc */

var SHEET_NAME = "Raw";

function doGet(e) {
  var lock = LockService.getPublicLock()
  lock.waitLock(30000);  // wait 30 seconds before conceding defeat.
  
  try {
    var doc = SpreadsheetApp.getActiveSpreadsheet();
    var sheet = doc.getSheetByName(SHEET_NAME);
    
    var headRow = 1;
    var headers = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getValues()[0];
    var nextRow = sheet.getLastRow() + 1; // get next row
    var row = [];
    // loop through the header columns
    for (var i in headers){
      if (headers[i] == "time") {
        // always use server-time for this column
        row.push(new Date());
      } else { // else use header name to get data
        row.push(e.parameter[headers[i]]);
      }
    }
    sheet.getRange(nextRow, 1, 1, row.length).setValues([row]);
    return ContentService.createTextOutput("Ok");
  } catch(e) {
    // if error return this
    return ContentService.createTextOutput('Error:' + JSON.stringify(e));
  } finally { // release lock
    lock.releaseLock();
  }
}