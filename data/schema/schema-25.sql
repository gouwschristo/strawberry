-- When each artist was last walked by the library indexer.
-- Replaces the serialised QVariantMap that used to live in the settings file.
CREATE TABLE IF NOT EXISTS youtubemusic_index (
  artist_id TEXT NOT NULL PRIMARY KEY,
  indexed_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_youtubemusic_index_at ON youtubemusic_index (indexed_at);
