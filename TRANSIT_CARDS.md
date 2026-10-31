# Transit cards on watched channels

Watch a transit bot's channel (for example `#transit`, answered by [BusBot 2.0](https://github.com/jade-on-mesh/BusBot-2.0)) under **Team → Watch** and its replies about a stop go on the map and into the Watch list as transit cards, as in MeshCore Open TAK's Watch tab (`documentation/transit-watch.md` there).

BusBot ends a channel reply about a stop with a line like `📍 47.60970,-122.33830 · 1120`: where the stop is and its number. Wadamesh reads the whole reply back into a card:

- **The list** shows the stop (or `Arrive 8:25 · …` for a trip plan), its distance, and the first arrivals with countdowns as of now: `11  4 min (8:14)`.
- **Tap it** for everything: the stop's number and agency, each route with its mode (Bus, RapidRide, Rail, Train, Ferry), how long until it comes and its clock time, live or timetable, alerts (`! …`), an offline notice, other agencies' stops with the same number, and for `go` replies each ride and walk as steps with "leaves in …". **Map** and **Hound** work as for any watched report.
- A reply the bot split over two messages is joined back together (same sender, within two minutes); it appears once its last part, with the position, is in.
- On a channel named for transit, the bot's lone alert posts are read too, though without a position they don't go on the map.

Countdowns run from when the reply was sent; clock times need the radio's clock to be set. Replies with no stop (help, a search with no results) stay ordinary channel messages.

The parser is `src/ui-touch/TransitFeed.h` (host test `test/test_transit_feed.cpp`, samples shared with MeshCore Open TAK's `test/team/transit_watch_test.dart`); the glue is in `src/ui-touch/TeamUI.inc`.
