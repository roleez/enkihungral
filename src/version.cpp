#include <Arduino.h>

#define VERZIO __DATE__
#define IDO __TIME__
const char *inputDate = VERZIO;
char convertedDate[15];

void convertTime(char *output) {
  //const char alphabet[] PROGMEM = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const char ABC[]=
      "AAABBCCCDDEEFFFGGHHIIIJJKKLLLMMNNNOOPPQQQRRSSTTTUUVVWWWXXYYZ"; //DevC++ generálta

  // Az __TIME__ formátuma: "hh:mm:ss"
  int hours = (IDO[0] - '0') * 10 + (IDO[1] - '0');
  int minutes = (IDO[3] - '0') * 10 + (IDO[4] - '0');

  //int totalMinutes = hours * 60 + minutes;

  // Lineáris elosztás a karaktertömbre
  //int index = totalMinutes * 26 / (24 * 60);

  //float nov = (float)minutes * 25.0 / 59.0; // --ebből generálta

  // Az első karakter az óra, a második karakter a perc
  char betu = 'A'+(hours%26);
  output[0] = betu;
  output[1] = ABC[minutes % 60];

  // Lezáró null karakter hozzáadása a karaktertömb végéhez
  output[2] = '\0';
}

int getMonth(const char *month) {
  char honapok[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  for (int i = 0; i < 12; ++i) {
    if (strcmp(month, honapok[i]) == 0) {
      return i + 1;
    }
  }

  return 1; // Alapértelmezett hónap, ha nincs egyezés
}

void convertDate(const char *inputDate, char *outputDate) {
  int month, day, year;

  // Az inputDate formátuma: "Mon DD YYYY"
  // Példa: "Nov 26 2023"

  // A dátumot tartalmazó részek kinyerése
  char inputCopy[32];
  strncpy(inputCopy, inputDate, sizeof(inputCopy) - 1);
  inputCopy[sizeof(inputCopy) - 1] =
      '\0'; // Biztosítjuk a lezáró null karaktert

  char *token = strtok(inputCopy, " ");
  if (token != nullptr) {
    // Az első rész: hónap
    month = getMonth(token);

    // A második rész: nap
    token = strtok(nullptr, " ");
    if (token != nullptr) {
      day = atoi(token);

      // A harmadik rész: év
      token = strtok(nullptr, " ");
      if (token != nullptr) {
        year = atoi(token);
      }
    }
  }

  // Az outputDate formátuma: "yyyymmdd"
  sprintf(outputDate, "%04d%02d%02d", year, month, day);
}
/* példa:
  Serial.println(inputDate);
  convertDate(inputDate, convertedDate);
  printf("Convert Date: %s\n", convertedDate);
*/


