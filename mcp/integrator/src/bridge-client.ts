import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";

type Pending = {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
};

export class TracyBridgeClient {
  private proc: ChildProcessWithoutNullStreams;
  private buf = "";
  private pending = new Map<number, Pending>();
  private nextId = 0;

  constructor(exePath: string) {
    this.proc = spawn(exePath, [], { stdio: ["pipe", "pipe", "pipe"] });
    this.proc.stdout.setEncoding("utf8");
    this.proc.stdout.on("data", (chunk: string) => this.onData(chunk));
    this.proc.stderr.on("data", () => {
      /* stderr from bridge is unexpected; ignore */
    });
    this.proc.on("error", (err) => {
      for (const p of this.pending.values()) {
        clearTimeout(p.timer);
        p.reject(err);
      }
      this.pending.clear();
    });
    this.proc.on("close", (code) => {
      const e = new Error(`tracy-mcp-bridge exited with ${code}`);
      for (const p of this.pending.values()) {
        clearTimeout(p.timer);
        p.reject(e);
      }
      this.pending.clear();
    });
  }

  private onData(chunk: string): void {
    this.buf += chunk;
    let idx: number;
    while ((idx = this.buf.indexOf("\n")) !== -1) {
      const line = this.buf.slice(0, idx).replace(/\r$/, "");
      this.buf = this.buf.slice(idx + 1);
      if (!line.trim()) continue;
      let msg: { id?: number; result?: unknown; error?: unknown };
      try {
        msg = JSON.parse(line) as typeof msg;
      } catch {
        continue;
      }
      if (msg.id === undefined || msg.id === null) continue;
      const id = Number(msg.id);
      const p = this.pending.get(id);
      if (!p) continue;
      this.pending.delete(id);
      clearTimeout(p.timer);
      if (msg.error !== undefined) {
        p.reject(new Error(typeof msg.error === "string" ? msg.error : JSON.stringify(msg.error)));
      } else {
        p.resolve(msg.result);
      }
    }
  }

  request(method: string, params: Record<string, unknown> = {}, timeoutMs = 120_000): Promise<unknown> {
    const id = ++this.nextId;
    const payload = `${JSON.stringify({ id, method, params })}\n`;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`bridge timeout: ${method}`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.proc.stdin.write(payload, (err) => {
        if (err) {
          clearTimeout(timer);
          this.pending.delete(id);
          reject(err);
        }
      });
    });
  }

  async ping(): Promise<boolean> {
    try {
      const r = (await this.request("ping", {}, 5_000)) as Record<string, unknown>;
      return r?.ok === true && r?.pong === true;
    } catch {
      return false;
    }
  }

  kill(): void {
    this.proc.kill();
  }
}
