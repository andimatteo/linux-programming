Just a very simple echo server that uses `io_uring` API.

---

To build with Docker on MacOS:

```bash
docker build --platform linux/amd64 -t echo .
```

To run it

```bash
docker run --security-opt seccomp=unconfined -it echo
```

