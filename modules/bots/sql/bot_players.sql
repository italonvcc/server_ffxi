-- Bot players: see modules/bots/cpp/bot_players.cpp
-- Both tables are also created automatically by the map server and the dashboard.
-- Keep in sync with ensureTables() in bot_players.cpp.

-- One row per bot character. Live columns are written by the map server.
CREATE TABLE IF NOT EXISTS `bot_players` (
  `charid` INT(10) UNSIGNED NOT NULL,
  `autospawn` TINYINT(1) UNSIGNED NOT NULL DEFAULT 1,
  `online` TINYINT(1) UNSIGNED NOT NULL DEFAULT 0,
  `zone` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,
  `pos_x` FLOAT NOT NULL DEFAULT 0,
  `pos_y` FLOAT NOT NULL DEFAULT 0,
  `pos_z` FLOAT NOT NULL DEFAULT 0,
  `hp` INT(10) UNSIGNED NOT NULL DEFAULT 0,
  `max_hp` INT(10) UNSIGNED NOT NULL DEFAULT 0,
  `mp` INT(10) UNSIGNED NOT NULL DEFAULT 0,
  `max_mp` INT(10) UNSIGNED NOT NULL DEFAULT 0,
  `status_updated` DATETIME NULL DEFAULT NULL,
  `created` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Added with the AI. home_* is where the bot stands when it has nothing to do
-- (home_zone = 0: not set yet). ai_state (idle | follow | wait | return) and leader are live columns.
ALTER TABLE `bot_players`
  ADD COLUMN IF NOT EXISTS `home_zone` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `home_x` FLOAT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `home_y` FLOAT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `home_z` FLOAT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `home_rot` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `ai_state` VARCHAR(8) NOT NULL DEFAULT 'idle',
  ADD COLUMN IF NOT EXISTS `leader` VARCHAR(15) NOT NULL DEFAULT '';

-- Added with combat (ai_state also takes fight | rest | dead). Live columns: the main job's
-- level, its exp, and the exp the next level needs.
ALTER TABLE `bot_players`
  ADD COLUMN IF NOT EXISTS `level` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `exp` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `exp_next` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0;

-- Command queue, run by the map server every time server tick (2.4s).
-- command: create | spawn | despawn | summon | autospawn | delete | invite
--          (testing aids: move | puppet | engage)
-- status:  pending | done | failed
CREATE TABLE IF NOT EXISTS `bot_commands` (
  `id` INT(10) UNSIGNED NOT NULL AUTO_INCREMENT,
  `command` VARCHAR(16) NOT NULL,
  `charid` INT(10) UNSIGNED NULL DEFAULT NULL,
  `name` VARCHAR(15) NULL DEFAULT NULL,
  `race` TINYINT(3) UNSIGNED NULL DEFAULT NULL,
  `face` TINYINT(3) UNSIGNED NULL DEFAULT NULL,
  `size` TINYINT(3) UNSIGNED NULL DEFAULT NULL,
  `job` TINYINT(3) UNSIGNED NULL DEFAULT NULL,
  `level` TINYINT(3) UNSIGNED NULL DEFAULT NULL,
  `nation` TINYINT(3) UNSIGNED NULL DEFAULT NULL,
  `autospawn` TINYINT(1) UNSIGNED NULL DEFAULT NULL,
  `anchor_charid` INT(10) UNSIGNED NULL DEFAULT NULL,
  `zone` SMALLINT(5) UNSIGNED NULL DEFAULT NULL,
  `pos_x` FLOAT NULL DEFAULT NULL,
  `pos_y` FLOAT NULL DEFAULT NULL,
  `pos_z` FLOAT NULL DEFAULT NULL,
  `status` VARCHAR(8) NOT NULL DEFAULT 'pending',
  `result` VARCHAR(255) NULL DEFAULT NULL,
  `created` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `processed` DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `status` (`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
