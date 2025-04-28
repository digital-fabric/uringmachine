# 2025-04-28 Version 0.7

- Add `#shutdown`

# 2025-04-23 Version 0.6.1

- Improve `#snooze` to prevent op completion starvation

# 2025-04-23 Version 0.6

- Add `#periodically` for multishot timeout
- Add `UM::Actor` class
- Add `#prep_timeout` and `AsyncOp`

# 2024-11-14 Version 0.5

- Add `#waitpid`
- Add `UM.pipe`, `UM.kernel_version`
- Add `#open`
- Fix `#spin`
- Fix handling of signal interruption.
- Reimplement and simplify um_op
- Add `UM::Queue`, `#push`, `#pop`, `#shift`, `#unshift`
- Add `UM::Mutex`, `#synchronize`
- Add `#recv_each`
- Add `#getsockopt`, `#setsockopt`
- Simplify and improve op management

# 2024-10-06 Version 0.4

- Add socket constants
- Add `#bind`, `#listen`
- Add `#spin`
- Fix bugs in multishot read and other ops
- Add `#recv`, `#send`
- Add `#socket`, `#connect`

# 2024-10-04 Version 0.3

- Fix race condition affecting `#timeout` and `#sleep`.
- Add `#accept_each`
- Add `#accept`

# 2024-10-03 Version 0.2

- Remove old IOU code.
- Add `#read_each`
- Add `#read`

# 2024-10-03 Version 0.1

The basic fiber scheduling stuff

- `#schedule`, `#interrupt`
- `#snooze`, `#yield`
- `#timeout`, `#sleep`
