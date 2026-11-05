# Child processes

The exact v0.3 declarations and focused examples are indexed in the
[standard-library reference](standard_library.md#std-process-command).

`std::process::command` owns command construction, child handles, pipe
endpoints, and exit status. `std::process::errors` contains the portable error
family used at this host boundary.

```flecha
@imports :: lib::std::process::command;

childCommand := command::Command::init("flecha".string());
childCommand.arg("--version".string());

child, spawnError := childCommand.spawn();
if spawnError != none {
    return spawnError;
}

status, waitError := child.wait();
if waitError != none {
    return waitError;
}
```

`Command` owns its executable, arguments, environment overrides, and optional
working directory. Adding one of those values transfers or copies ownership as
declared by the method signature. Failure to grow command storage leaves the
previous command usable. `spawn()` borrows the finished configuration and
returns a `ChildProcess` owning the native process and configured pipe handles.

Spawning, pipe reads and writes, and `wait()` cross the operating-system
boundary and may block. `tryWait()` is the nonblocking status probe. Closing a
pipe or child handle is explicit where the API exposes `close`; normal scope
cleanup closes any remaining owned native resource exactly once. A child exit
code is data in `ExitStatus`, not a Flecha error. Spawn, pipe, wait, invalid
configuration, and unsupported-host failures use `ProcessError` or the wider
`error` return declared by the operation.

`ExitStatus` distinguishes normal exit from signal or other abnormal host
termination. Numeric details are host-derived and should not be treated as a
portable signal enumeration. Argument quoting, executable lookup, inherited
environment behavior, and available process features follow the documented
Windows or POSIX runtime boundary; no shell is inserted automatically.

The [`workspace_guard` north-star](../examples/workspace_guard/README.md)
builds a command from captured CLI arguments, starts it only after verification,
waits for it, and maps normal and abnormal termination into stable application
exit behavior.
