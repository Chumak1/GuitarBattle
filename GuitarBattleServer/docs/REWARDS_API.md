# Scoring normalization and anti-farm rewards

Revision: migration `0030_scoring_normalization` (down: `0029_character_unlocks`).
Client: **1.8.0+**. Public rules: `GET /api/v1/gameplay/rules`.

Everything below is computed server-side. A client sends facts only (score,
accuracy, note qualities, completion, played/total notes); it never sends XP, a
multiplier or a character id.

## Normalization

```
completion  = effective_completion(completion_percent, notes_total, notes_played)
ratio       = completion / 100
normalized  = round(actual_score / ratio)     if ratio*100 >= 10 and notes_total > 0
            = NULL                            otherwise
```

* `ratio` can only be lowered by the note counters, so a client cannot claim to
  have played more than it did.
* The multiplier is capped at `NORMALIZATION_MAX_MULTIPLIER` (10), i.e.
  `normalized <= actual * 10`.
* The actual score is stored untouched and stays the value used everywhere else
  (leaderboards, personal bests, XP inputs).
* Stored in `scores.normalized_score` (ranked solo rows) and
  `duel_participants.normalized_score` (online duels) and
  `campaign_stage_runs.normalized_score` (Battle).

## Battle (Tower riffs)

`POST /api/v1/campaigns/{campaign_id}/stages/{position}/result`

```
XP = progression.xp_for_attempt(accuracy, song.difficulty, song.bpm,
                                song.duration_seconds, completion,
                                repeat_multiplier = tower_factor)
```

The formula and every coefficient are the existing solo ones
(`XP_BASE=100`, difficulty/tempo/content factors, clamp `3..600`, and
`MIN_COMPLETION_FOR_XP=50`). Only the repeat multiplier is new:

```
tower_factor = 1.0                                  if completions_before == 0
             = max(TOWER_REPEAT_FLOOR,
                   TOWER_REPEAT_DECAY ** completions_before)   otherwise
```

`completions_before` is `user_campaign_progress.completion_count`: the number of
**full** tower completions before this run. So every stage of the first run is
full XP, the whole second run is 50%, the third 25%, etc.

Other rules:

* Only `won=true` earns tower XP (a loss stores the run and the normalized score
  but awards 0 and resets the run to the first stage: `best_stage = 0`, while
  `completed_at` and `completion_count` are kept).
* `position == best_stage % stage_count`; otherwise `409 stage_not_current`.
  `best_stage` grows on a win and resets on a loss, so a completed tower
  (`best_stage == stage_count`) starts its next run at stage 0 without losing
  the completion record. No published stages → `409 no_stages`.
* `(user_id, run_id)` is unique. A retried report returns the stored row with
  `already_processed=true`; `xp_total` and progress do not change.
* PB bonus: `PB_BONUS_FACTOR (0.5) × base XP` once, only when the accuracy is
  strictly higher than the stored best of that `(campaign, position)`.
* Achievements reuse the solo block (`progression.award_attempt_achievements`)
  with facts from the run; owned achievements are never re-awarded. A lost stage
  does not evaluate achievements.
* Completion transitions grant `campaigns.reward_character_id` idempotently via
  the existing character service; `unlocked_character` is filled only when it
  was newly inserted.

## Online duels

Elo is unchanged (`duel_rules.apply_elo`, K=32). XP:

```
base   = duel_rules.xp_for_duel(perfect, great, good, result)
XP     = round(base * progression.difficulty_factor(song.difficulty)
                     * online_repeat_factor(finished_duels_of_song_today))
PB     = round(base * PB_BONUS_FACTOR)   # only on a strict accuracy improvement
```

* `online_repeat_factor` uses `ONLINE_REPEAT_FACTORS = (1.0, 0.5, 0.25, 0.1)`
  by finished online duels of this player on this song in the current UTC day,
  then `ONLINE_REPEAT_FLOOR` (0.05).
* `duel_song_bests(user_id, song_id)` keeps `best_accuracy`/`best_score`; the
  accuracy is `duel_rules.accuracy_factor(...) * 100`, the score is
  `perfect*100 + great*85 + good*70`.
* `duel_participants.xp_breakdown` (JSONB) stores the audit snapshot;
  `xp_applied_at` makes a second `_apply_xp` for the same participant a no-op.
* `duel_participants.score_points` / `normalized_score` are additive fields in
  the participant payload.

## Configuration

`app/services/reward_config.py`, env-overridable, read once at import:

| Variable | Default |
|---|---|
| `TOWER_REPEAT_DECAY` | `0.5` |
| `TOWER_REPEAT_FLOOR` | `0.05` |
| `ONLINE_REPEAT_FACTORS` | `1.0,0.5,0.25,0.1` |
| `ONLINE_REPEAT_FLOOR` | `0.05` |
| `PB_BONUS_FACTOR` | `0.5` |
| `NORMALIZATION_MIN_COMPLETION` | `10.0` |
| `NORMALIZATION_MAX_MULTIPLIER` | `10.0` |

`MIN_COMPLETION_FOR_XP` remains in `app/services/progression.py` and is **not**
changed (50%).

## Tests

`tests/test_scoring_normalization.py` covers normalization, the 50% threshold,
Battle XP/difficulty/quality, repeat decay with the floor, idempotent `run_id`,
strict PB bonus, online repeat/difficulty/PB/once-only XP, Practice without
rewards and the public rules endpoint.
