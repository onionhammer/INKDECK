//! Times for schedules (the `schedule` tool): a time of day on some days of the week, in the
//! daemon machine's local time, and the next run after a moment.

use chrono::{Datelike, Local, LocalResult, NaiveDateTime, TimeZone};
use serde_json::Value;

/// Monday to Sunday.
pub const ALL_DAYS: u8 = 0b111_1111;
pub const WEEKDAYS: u8 = 0b001_1111;
pub const WEEKENDS: u8 = 0b110_0000;
const NAMES: [&str; 7] = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"];

/// "07:00" (or "7:00", "7") as hours and minutes.
pub fn parse_at(text: &str) -> Result<(u32, u32), String> {
    let text = text.trim();
    let (h, m) = match text.split_once(':') {
        Some((h, m)) => (h, m),
        None => (text, "0"),
    };
    match (h.trim().parse::<u32>(), m.trim().parse::<u32>()) {
        (Ok(h), Ok(m)) if h < 24 && m < 60 => Ok((h, m)),
        _ => Err(format!(
            "invalid input: at is a 24-hour time such as \"07:00\", not {text:?}"
        )),
    }
}

/// "07:00" for hours and minutes.
pub fn at_text((h, m): (u32, u32)) -> String {
    format!("{h:02}:{m:02}")
}

/// The days: "daily", "weekdays", "weekends", or a list of "mon" … "sun". None: daily.
pub fn parse_days(value: Option<&Value>) -> Result<u8, String> {
    let named = |name: &str| -> Result<u8, String> {
        let name = name.trim().to_lowercase();
        match name.as_str() {
            "daily" | "every day" | "everyday" | "all" => Ok(ALL_DAYS),
            "weekdays" => Ok(WEEKDAYS),
            "weekends" => Ok(WEEKENDS),
            _ => NAMES
                .iter()
                .position(|n| name.starts_with(n))
                .map(|i| 1 << i)
                .ok_or_else(|| {
                    format!(
                        "invalid input: days is daily, weekdays, weekends or a list of mon … \
                         sun, not {name:?}"
                    )
                }),
        }
    };
    let days = match value {
        None | Some(Value::Null) => ALL_DAYS,
        Some(Value::String(name)) => named(name)?,
        Some(Value::Array(names)) => {
            let mut days = 0;
            for name in names {
                let Some(name) = name.as_str() else {
                    return Err("invalid input: days lists day names".into());
                };
                days |= named(name)?;
            }
            days
        }
        Some(_) => return Err("invalid input: days is a name or a list of day names".into()),
    };
    if days == 0 {
        return Err("invalid input: give at least one day".into());
    }
    Ok(days)
}

/// The days as a board shows them: Monday first, a letter for each day it runs, `.` for the
/// others ("MTWTF..").
pub fn days_text(days: u8) -> String {
    "MTWTFSS"
        .chars()
        .enumerate()
        .map(|(i, c)| if days & (1 << i) != 0 { c } else { '.' })
        .collect()
}

/// The days in words, for the model: "weekdays", "daily", "mon, thu".
pub fn days_words(days: u8) -> String {
    match days {
        ALL_DAYS => "daily".into(),
        WEEKDAYS => "weekdays".into(),
        WEEKENDS => "weekends".into(),
        _ => (0..7)
            .filter(|i| days & (1 << i) != 0)
            .map(|i| NAMES[i])
            .collect::<Vec<_>>()
            .join(", "),
    }
}

/// The first run after `after` (Unix seconds): `at` local time on one of `days`. A time that a
/// clock change skips runs an hour later; one it repeats runs the first time.
pub fn next_run(at: (u32, u32), days: u8, after: u64) -> u64 {
    let days = if days == 0 { ALL_DAYS } else { days };
    let Some(start) = Local.timestamp_opt(after as i64, 0).single() else {
        return after + 86_400;
    };
    let today = start.date_naive();
    for ahead in 0..=8 {
        let Some(date) = today.checked_add_days(chrono::Days::new(ahead)) else {
            break;
        };
        if days & (1 << date.weekday().num_days_from_monday()) == 0 {
            continue;
        }
        let Some(naive) = date.and_hms_opt(at.0, at.1, 0) else {
            continue;
        };
        if let Some(when) = resolve(naive)
            && when > after
        {
            return when;
        }
    }
    after + 86_400
}

/// A local date and time as Unix seconds.
fn resolve(naive: NaiveDateTime) -> Option<u64> {
    let when = match Local.from_local_datetime(&naive) {
        LocalResult::Single(t) => t,
        LocalResult::Ambiguous(first, _) => first,
        LocalResult::None => match Local.from_local_datetime(&(naive + chrono::Duration::hours(1)))
        {
            LocalResult::Single(t) | LocalResult::Ambiguous(t, _) => t,
            LocalResult::None => return None,
        },
    };
    u64::try_from(when.timestamp()).ok()
}

/// A moment as the local time of day, "07:00", and its weekday, "Mon".
pub fn local_text(when: u64) -> String {
    match Local.timestamp_opt(when as i64, 0).single() {
        Some(t) => t.format("%a %H:%M").to_string(),
        None => when.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::Timelike;
    use serde_json::json;

    #[test]
    fn times_and_days() {
        assert_eq!(parse_at("07:00"), Ok((7, 0)));
        assert_eq!(parse_at(" 7:30 "), Ok((7, 30)));
        assert_eq!(parse_at("18"), Ok((18, 0)));
        assert!(parse_at("24:00").is_err() && parse_at("7pm").is_err());
        assert_eq!(at_text((7, 5)), "07:05");

        assert_eq!(parse_days(None), Ok(ALL_DAYS));
        assert_eq!(parse_days(Some(&json!("weekdays"))), Ok(WEEKDAYS));
        assert_eq!(
            parse_days(Some(&json!(["Mon", "thursday"]))),
            Ok(0b000_1001)
        );
        assert!(parse_days(Some(&json!("someday"))).is_err());
        assert!(parse_days(Some(&json!([]))).is_err());
        assert_eq!(days_text(WEEKDAYS), "MTWTF..");
        assert_eq!(days_text(0b000_1001), "M..T...");
        assert_eq!(days_words(WEEKDAYS), "weekdays");
        assert_eq!(days_words(0b000_1001), "mon, thu");
    }

    #[test]
    fn the_next_run_is_on_one_of_its_days_at_its_time() {
        let now = 1_795_190_000; // a Thursday
        for (at, days) in [
            ((7, 0), ALL_DAYS),
            ((23, 59), WEEKENDS),
            ((0, 0), 0b000_0001),
        ] {
            let next = next_run(at, days, now);
            assert!(next > now && next <= now + 8 * 86_400, "{next}");
            let local = Local.timestamp_opt(next as i64, 0).unwrap();
            assert_eq!((local.hour(), local.minute()), at);
            assert!(days & (1 << local.weekday().num_days_from_monday()) != 0);
            // From that run, the next is a later one.
            assert!(next_run(at, days, next) > next);
        }
    }
}
