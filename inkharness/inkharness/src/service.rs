//! `inkharness service`: runs `inkharness serve` from a folder as a systemd user service, so
//! it starts at boot (with lingering, before anyone logs in) and again after a crash.
//!
//! `install` writes `~/.config/systemd/user/inkharness.service` for the folder it's run in
//! (or `-C DIR`), this binary and the PATH it was started with (MCP servers, `claude`, `npx`),
//! then enables and (re)starts it. Running it again replaces the unit, so it's also how a
//! service moves to another folder. Linux only: it drives `systemctl --user`, `loginctl` and
//! `journalctl`.

use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result, bail};
use clap::Subcommand;

use crate::place;

const UNIT: &str = "inkharness.service";

#[derive(Subcommand)]
pub enum Action {
    /// Run `inkharness serve` from this folder (or -C DIR) at every boot, and start it now.
    Install,
    /// Stop the service and remove it.
    Uninstall,
    /// Restart it (after an update, or a change to its config).
    Restart,
    /// Whether it's running, and its last lines.
    Status,
    /// Follow its log.
    Logs,
}

pub fn run(action: Action, config: Option<&Path>) -> Result<()> {
    if !cfg!(target_os = "linux") {
        bail!("`inkharness service` drives systemd, so it runs on Linux only");
    }
    match action {
        Action::Install => install(config),
        Action::Uninstall => uninstall(),
        Action::Restart => systemctl(&["restart", UNIT]),
        Action::Status => {
            // Its exit code says "not running"; the text says it better.
            let _ = Command::new("systemctl")
                .args(["--user", "status", "--no-pager", UNIT])
                .status();
            Ok(())
        }
        Action::Logs => {
            let _ = Command::new("journalctl")
                .args(["--user", "-u", UNIT, "-n", "100", "-f"])
                .status()
                .context("running journalctl")?;
            Ok(())
        }
    }
}

fn install(config: Option<&Path>) -> Result<()> {
    let dir = std::env::current_dir()?;
    let config = config.map(std::path::absolute).transpose()?;
    // A config that doesn't load would only restart every few seconds: say so now.
    place::load(&dir, config.as_deref()).context("the config here doesn't load")?;

    let exe = std::env::current_exe()
        .and_then(|e| e.canonicalize())
        .context("finding the inkharness binary")?;
    let env = Env {
        path: std::env::var("PATH").unwrap_or_default(),
        home: std::env::var("INKHARNESS_HOME").ok().filter(|h| !h.is_empty()),
    };
    let file = unit_file()?;
    std::fs::create_dir_all(file.parent().unwrap())?;
    std::fs::write(&file, unit(&exe, &dir, config.as_deref(), &env))
        .with_context(|| format!("writing {}", file.display()))?;
    println!("wrote {}", file.display());

    systemctl(&["daemon-reload"])?;
    systemctl(&["enable", UNIT])?;
    // restart, not start: a second install picks up the new unit.
    systemctl(&["restart", UNIT])?;

    // Without lingering, user services start at login and stop at logout.
    let lingering = Command::new("loginctl")
        .arg("enable-linger")
        .status()
        .is_ok_and(|s| s.success());
    if !lingering {
        let user = std::env::var("USER").unwrap_or_else(|_| "$USER".into());
        println!(
            "Couldn't turn on lingering, so it starts only once you log in. For boot: sudo loginctl enable-linger {user}"
        );
    }
    println!("inkharness serve runs from {} now, and at every boot.", dir.display());
    println!(
        "The service sees only PATH from this shell: keys go in .env here or in the global folder."
    );
    println!("inkharness service status | logs | restart | uninstall");
    Ok(())
}

fn uninstall() -> Result<()> {
    let file = unit_file()?;
    if !file.exists() {
        bail!("there's no {} to remove", file.display());
    }
    // Stopping fails if it's already stopped; removing it is what matters.
    let _ = systemctl(&["disable", "--now", UNIT]);
    std::fs::remove_file(&file).with_context(|| format!("removing {}", file.display()))?;
    systemctl(&["daemon-reload"])?;
    println!("removed {}", file.display());
    Ok(())
}

/// `$XDG_CONFIG_HOME/systemd/user/inkharness.service`, else under `~/.config`.
fn unit_file() -> Result<PathBuf> {
    let config = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::home_dir().map(|h| h.join(".config")))
        .context("no home folder for the unit")?;
    Ok(config.join("systemd").join("user").join(UNIT))
}

fn systemctl(args: &[&str]) -> Result<()> {
    let status = Command::new("systemctl")
        .arg("--user")
        .args(args)
        .status()
        .context("running systemctl (is this a systemd system?)")?;
    if !status.success() {
        bail!("systemctl --user {} failed", args.join(" "));
    }
    Ok(())
}

/// What the service gets from the shell that installed it.
struct Env {
    path: String,
    home: Option<String>,
}

fn unit(exe: &Path, dir: &Path, config: Option<&Path>, env: &Env) -> String {
    let mut exec = format!("{} serve", quote(&exe.to_string_lossy()));
    if let Some(config) = config {
        exec += &format!(" --config {}", quote(&config.to_string_lossy()));
    }
    let mut environment = format!("Environment={}\n", quote(&format!("PATH={}", env.path)));
    if let Some(home) = &env.home {
        environment += &format!("Environment={}\n", quote(&format!("INKHARNESS_HOME={home}")));
    }
    format!(
        "# Written by `inkharness service install`; running it again replaces this file.\n\
         [Unit]\n\
         Description=inkharness serve (the daemon INKDECK boards talk to)\n\
         \n\
         [Service]\n\
         WorkingDirectory={dir}\n\
         ExecStart={exec}\n\
         {environment}\
         Restart=always\n\
         RestartSec=5\n\
         \n\
         [Install]\n\
         WantedBy=default.target\n",
        dir = dir.to_string_lossy().replace('%', "%%"),
    )
}

/// One word for systemd: in double quotes, with `%` (its specifiers) doubled.
fn quote(s: &str) -> String {
    let s = s.replace('\\', "\\\\").replace('"', "\\\"").replace('%', "%%");
    format!("\"{s}\"")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unit_runs_serve_from_the_folder() {
        let env = Env {
            path: "/usr/bin:/bin".into(),
            home: None,
        };
        let text = unit(
            Path::new("/home/me/.cargo/bin/inkharness"),
            Path::new("/home/me/ink"),
            None,
            &env,
        );
        assert!(text.contains("WorkingDirectory=/home/me/ink\n"));
        assert!(text.contains("ExecStart=\"/home/me/.cargo/bin/inkharness\" serve\n"));
        assert!(text.contains("Environment=\"PATH=/usr/bin:/bin\"\n"));
        assert!(!text.contains("INKHARNESS_HOME"));
        assert!(text.contains("Restart=always\n"));
        assert!(text.contains("WantedBy=default.target\n"));
    }

    #[test]
    fn unit_quotes_what_systemd_would_split_or_expand() {
        let env = Env {
            path: "/a b:/c".into(),
            home: Some("/h/50%".into()),
        };
        let text = unit(
            Path::new("/opt/ink harness/inkharness"),
            Path::new("/srv/100% ink"),
            Some(Path::new("/srv/my \"ink\".toml")),
            &env,
        );
        assert!(text.contains("WorkingDirectory=/srv/100%% ink\n"));
        assert!(text.contains(
            "ExecStart=\"/opt/ink harness/inkharness\" serve --config \"/srv/my \\\"ink\\\".toml\"\n"
        ));
        assert!(text.contains("Environment=\"PATH=/a b:/c\"\n"));
        assert!(text.contains("Environment=\"INKHARNESS_HOME=/h/50%%\"\n"));
    }
}
