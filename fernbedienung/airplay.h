/* AirPlay-Empfaenger fuer die Bildschirmsynchronisierung (nach dem Vorbild von UxPlay, GPL-3) */
#ifndef AIRPLAY_H
#define AIRPLAY_H

typedef void (*ApMeld)(const char *f, ...);

/* startet den RTSP-Dienst (Port wunsch, sonst frei), gibt den Port zurueck oder -1 */
int airplay_start(int wunsch, const unsigned char ed_sk[64], const unsigned char ed_pk[32], const unsigned char mac[6],
                  const char *name, const char *kennung, const char *anzeige_pfad, ApMeld meld, int laut);

/* TXT-Eintraege fuer Bonjour (Laenge zurueck) */
int airplay_txt(unsigned char *aus, int max);
int raop_txt(unsigned char *aus, int max);
/* Instanzname fuer _raop._tcp: "MAC@Name" */
const char *raop_name(void);

#endif
