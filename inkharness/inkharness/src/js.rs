//! The run_js tool: JavaScript in a QuickJS sandbox, for arithmetic and dates, or to chain
//! several home tool calls in one step.
//!
//! The sandbox is a fresh QuickJS runtime per script with the language's built-ins only (no
//! files, network, timers or modules: QuickJS has none unless they are added), a memory and
//! stack limit, a deadline enforced by the interrupt handler, and a cap on tool calls. The home
//! tools are synchronous functions on a global `tools` object; each call blocks the script's
//! thread (a blocking-pool thread, not the async runtime) until the tool answers.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

use futures_util::future::BoxFuture;
use rquickjs::function::{Opt, Rest};
use rquickjs::{
    CatchResultExt, CaughtError, Context, Ctx, Exception, Function, Object, Runtime, Value,
};

/// What a script may use.
#[derive(Clone, Debug)]
pub struct Limits {
    pub timeout: Duration,
    pub memory_bytes: usize,
    pub max_tool_calls: usize,
}

/// Runs a tool by name with its input; `Err` is a message for the script.
pub type Caller = Arc<
    dyn Fn(String, serde_json::Value) -> BoxFuture<'static, Result<String, String>> + Send + Sync,
>;

/// Output returned to the model is cut to this many characters.
const MAX_OUTPUT: usize = 4000;
const STACK_BYTES: usize = 512 * 1024;

/// Runs `code`. Returns what it printed and the value of its last expression, or an error
/// message (with whatever it printed first).
pub async fn run(
    code: String,
    limits: Limits,
    tools: Vec<String>,
    call: Caller,
) -> Result<String, String> {
    let handle = tokio::runtime::Handle::current();
    let started = Instant::now();
    let outcome =
        tokio::task::spawn_blocking(move || run_blocking(&code, &limits, &tools, call, handle))
            .await
            .map_err(|e| format!("the script's thread failed: {e}"))?;
    tracing::info!(
        ms = started.elapsed().as_millis() as u64,
        ok = outcome.is_ok(),
        "run_js"
    );
    outcome
}

fn run_blocking(
    code: &str,
    limits: &Limits,
    tools: &[String],
    call: Caller,
    handle: tokio::runtime::Handle,
) -> Result<String, String> {
    let runtime = Runtime::new().map_err(|e| format!("no JavaScript runtime: {e}"))?;
    runtime.set_memory_limit(limits.memory_bytes);
    runtime.set_max_stack_size(STACK_BYTES);
    let deadline = Instant::now() + limits.timeout;
    runtime.set_interrupt_handler(Some(Box::new(move || Instant::now() > deadline)));
    let context = Context::full(&runtime).map_err(|e| format!("no JavaScript context: {e}"))?;

    let printed = Rc::new(RefCell::new(String::new()));
    let calls = Rc::new(RefCell::new(0usize));
    context.with(|ctx| {
        let setup = (|| -> rquickjs::Result<()> {
            let globals = ctx.globals();
            let print = {
                let printed = printed.clone();
                Function::new(
                    ctx.clone(),
                    print_fn(move |ctx, args| {
                        let line: Vec<String> = args.0.into_iter().map(|v| show(&ctx, v)).collect();
                        let mut out = printed.borrow_mut();
                        out.push_str(&line.join(" "));
                        out.push('\n');
                    }),
                )?
            };
            globals.set("print", print.clone())?;
            let console = Object::new(ctx.clone())?;
            console.set("log", print)?;
            globals.set("console", console)?;

            let tool_object = Object::new(ctx.clone())?;
            for name in tools {
                let (name_js, call, handle, calls) =
                    (name.clone(), call.clone(), handle.clone(), calls.clone());
                let max = limits.max_tool_calls;
                let f = Function::new(
                    ctx.clone(),
                    tool_fn(move |ctx, input| {
                        {
                            let mut n = calls.borrow_mut();
                            if *n >= max {
                                return Err(Exception::throw_message(
                                    &ctx,
                                    &format!("more than {max} tool calls"),
                                ));
                            }
                            *n += 1;
                        }
                        let input = match input.0 {
                            None => serde_json::json!({}),
                            Some(v) if v.is_undefined() || v.is_null() => serde_json::json!({}),
                            Some(v) => {
                                let text = ctx
                                    .json_stringify(v)?
                                    .map(|s| s.to_string())
                                    .transpose()?
                                    .unwrap_or_else(|| "{}".into());
                                serde_json::from_str(&text).unwrap_or(serde_json::json!({}))
                            }
                        };
                        tracing::info!(tool = %name_js, %input, "tool call from run_js");
                        let left = deadline.saturating_duration_since(Instant::now());
                        let result = handle
                            .block_on(tokio::time::timeout(left, call(name_js.clone(), input)));
                        match result {
                            Ok(Ok(text)) => Ok(text),
                            Ok(Err(message)) => Err(Exception::throw_message(&ctx, &message)),
                            Err(_) => {
                                Err(Exception::throw_message(&ctx, "the script ran out of time"))
                            }
                        }
                    }),
                )?;
                tool_object.set(name.as_str(), f)?;
            }
            globals.set("tools", tool_object)?;
            Ok(())
        })();
        setup.map_err(|e| format!("setting up the sandbox failed: {e}"))?;

        let result: Result<Value, CaughtError> = ctx.eval(code).catch(&ctx);
        let mut out = printed.borrow().clone();
        match result {
            Ok(value) => {
                if !value.is_undefined() {
                    out.push_str("=> ");
                    out.push_str(&show(&ctx, value));
                }
                if out.trim().is_empty() {
                    out = "(no output: print a value, or end with an expression)".into();
                }
                Ok(cut(out))
            }
            Err(error) => {
                let message = if Instant::now() > deadline {
                    format!("stopped after {} ms", limits.timeout.as_millis())
                } else {
                    error_message(&error)
                };
                let shown = if out.trim().is_empty() {
                    format!("Error: {message}")
                } else {
                    format!("{out}Error: {message}")
                };
                Err(cut(shown))
            }
        }
    })
}

// Closures given to Function::new take a context and values of one lifetime; these say so.
fn print_fn<F: for<'js> Fn(Ctx<'js>, Rest<Value<'js>>)>(f: F) -> F {
    f
}
fn tool_fn<F: for<'js> Fn(Ctx<'js>, Opt<Value<'js>>) -> rquickjs::Result<String>>(f: F) -> F {
    f
}

/// A value as text: strings as they are, everything else as JSON where it has a JSON form.
fn show<'js>(ctx: &Ctx<'js>, value: Value<'js>) -> String {
    if let Some(s) = value.as_string() {
        return s.to_string().unwrap_or_default();
    }
    if value.is_function() {
        return "[function]".into();
    }
    if let Ok(Some(s)) = ctx.json_stringify(value.clone())
        && let Ok(s) = s.to_string()
    {
        return s;
    }
    format!("{value:?}")
}

/// What the model sees when a script hits the memory limit.
const OUT_OF_MEMORY: &str = "the script ran out of memory";

fn error_message(error: &CaughtError<'_>) -> String {
    match error {
        // At the memory limit QuickJS throws an InternalError "out of memory", or null when
        // there isn't room left even for that error object (`JS_ThrowOutOfMemory`). Which one
        // depends on what the failed allocation left free, so it varies by platform: both
        // read the same to the model.
        CaughtError::Value(v) if v.is_null() => OUT_OF_MEMORY.into(),
        CaughtError::Exception(e) if is_out_of_memory(e) => OUT_OF_MEMORY.into(),
        CaughtError::Exception(e) => match (e.message(), e.stack()) {
            (Some(m), _) if !m.is_empty() => m,
            _ => error.to_string().trim().to_string(),
        },
        _ => error.to_string().trim().to_string(),
    }
}

/// QuickJS's own out-of-memory error, not a script's `new Error('out of memory')`.
fn is_out_of_memory(e: &Exception<'_>) -> bool {
    e.message().as_deref() == Some("out of memory")
        && e.get::<_, String>("name")
            .is_ok_and(|n| n == "InternalError")
}

fn cut(mut text: String) -> String {
    if text.chars().count() > MAX_OUTPUT {
        text = text.chars().take(MAX_OUTPUT).collect();
        text.push_str("\n(output cut)");
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    fn limits() -> Limits {
        Limits {
            timeout: Duration::from_millis(500),
            memory_bytes: 16 << 20,
            max_tool_calls: 3,
        }
    }

    /// `echo` returns its input as JSON; `fail` always fails.
    fn caller() -> Caller {
        Arc::new(|name, input| {
            Box::pin(async move {
                match name.as_str() {
                    "echo" => Ok(input.to_string()),
                    _ => Err("it broke".to_string()),
                }
            })
        })
    }

    async fn js(code: &str) -> Result<String, String> {
        run(
            code.into(),
            limits(),
            vec!["echo".into(), "fail".into()],
            caller(),
        )
        .await
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn expressions_and_print() {
        assert_eq!(js("0.17 * 2340").await.unwrap(), "=> 397.8");
        assert_eq!(
            js("print('a', 1, [2]); console.log({b: 3})").await.unwrap(),
            "a 1 [2]\n{\"b\":3}\n"
        );
        assert_eq!(
            js("const d = new Date(Date.UTC(2026, 10, 15, 15, 47)); new Date(d.getTime() + 158 * 60000).toISOString()")
                .await
                .unwrap(),
            "=> 2026-11-15T18:25:00.000Z"
        );
        assert!(js("let x = 1").await.unwrap().starts_with("(no output"));
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn tools_are_functions() {
        let out =
            js("const r = JSON.parse(tools.echo({speaker: 'media_player.kitchen'})); r.speaker")
                .await;
        assert_eq!(out.unwrap(), "=> media_player.kitchen");
        let out = js("try { tools.fail() } catch (e) { 'caught: ' + e.message }").await;
        assert_eq!(out.unwrap(), "=> caught: it broke");
        let err = js("for (let i = 0; i < 10; i++) tools.echo()")
            .await
            .unwrap_err();
        assert!(err.contains("more than 3 tool calls"), "{err}");
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn sandbox_limits() {
        let err = js("while (true) {}").await.unwrap_err();
        assert!(err.contains("stopped after 500 ms"), "{err}");
        let err = js("const a = []; while (true) a.push('x'.repeat(1000))")
            .await
            .unwrap_err();
        assert_eq!(err, "Error: the script ran out of memory");
        // Both of QuickJS's out-of-memory throws, whichever one the platform hit above.
        for thrown in ["throw new InternalError('out of memory')", "throw null"] {
            let err = js(thrown).await.unwrap_err();
            assert_eq!(err, "Error: the script ran out of memory", "{thrown}");
        }
        let err = js("throw new Error('out of memory')").await.unwrap_err();
        assert_eq!(err, "Error: out of memory");
        let err = js("function f() { return f() } f()").await.unwrap_err();
        assert!(err.contains("stack"), "{err}");
        for missing in ["require", "fetch", "setTimeout", "std", "os", "process"] {
            let out = js(&format!("typeof {missing}")).await.unwrap();
            assert_eq!(out, "=> undefined", "{missing}");
        }
        let err = js("print('before'); throw new Error('nope')")
            .await
            .unwrap_err();
        assert_eq!(err, "before\nError: nope");
    }
}
