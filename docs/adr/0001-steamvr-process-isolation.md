# Keep the SteamVR driver independent of tracking

The SteamVR driver owns session startup and supervision; tracking and the dashboard run in separate processes to contain failures and allow runtime replacement while SteamVR remains running. The loaded driver stays fixed across runtime updates, so its control protocol is a compatibility boundary. Manually started servers retain their lifetime ownership when SteamVR connects.
