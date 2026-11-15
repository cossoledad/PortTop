"""Small UTF-8 VT screen used to assert ncurses screen state, not output chunks.
Supports the xterm sequences emitted by this application's ncurses rendering.
"""
import re
import codecs

class Screen:
    def __init__(self, height=36, width=130):
        self.decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self.height, self.width = height, width
        self.rows = [[" "] * width for _ in range(height)]
        self.y = self.x = 0
        self.saved = (0, 0)
        self.pending = ""
        self.last = " "
        self.top, self.bottom = 0, height - 1
    def resize(self, height, width):
        self.rows = [(r[:width] + [" "] * max(0, width-len(r))) for r in self.rows[:height]]
        self.rows += [[" "] * width for _ in range(max(0, height-len(self.rows)))]
        self.height, self.width = height, width
        self.top, self.bottom = 0, height - 1
        self.y, self.x = min(self.y, height-1), min(self.x, width-1)
    def newline(self):
        if self.y == self.bottom:
            self.rows.pop(self.top)
            self.rows.insert(self.bottom, [" "] * self.width)
        else:
            self.y = min(self.y+1, self.height-1)
    def char(self, c):
        if self.x >= self.width:
            self.x = 0
            self.newline()
        self.rows[self.y][self.x] = c
        self.x += 1
        self.last = c
    def csi(self, body, op):
        private = body.startswith("?")
        nums = [int(n) if n else 0 for n in body.lstrip("?>").split(";")]
        n = nums[0] or 1
        if private:
            return
        if op in "Hf":
            self.y = min((nums[0] or 1)-1, self.height-1)
            self.x = min(((nums[1] if len(nums)>1 else 0) or 1)-1, self.width-1)
        elif op == "A": self.y = max(0, self.y-n)
        elif op == "B": self.y = min(self.height-1, self.y+n)
        elif op == "C": self.x = min(self.width-1, self.x+n)
        elif op == "D": self.x = max(0, self.x-n)
        elif op == "G": self.x = min(self.width-1, n-1)
        elif op == "d": self.y = min(self.height-1, n-1)
        elif op == "J":
            mode = nums[0]
            for y in range(self.height):
                for x in range(self.width):
                    if mode == 2 or (mode == 0 and (y,x) >= (self.y,self.x)) or (mode == 1 and (y,x) <= (self.y,self.x)):
                        self.rows[y][x] = " "
        elif op == "K":
            mode = nums[0]
            for x in range(self.width):
                if mode == 2 or (mode == 0 and x>=self.x) or (mode == 1 and x<=self.x): self.rows[self.y][x] = " "
        elif op == "X": self.rows[self.y][self.x:self.x+n] = [" "] * min(n, self.width-self.x)
        elif op == "P": self.rows[self.y] = (self.rows[self.y][:self.x] + self.rows[self.y][self.x+n:] + [" "]*n)[:self.width]
        elif op == "@": self.rows[self.y] = (self.rows[self.y][:self.x] + [" "]*n + self.rows[self.y][self.x:])[:self.width]
        elif op == "b":
            for _ in range(n): self.char(self.last)
        elif op == "r": self.top, self.bottom = max(0,(nums[0] or 1)-1), min(self.height-1, ((nums[1] if len(nums)>1 else 0) or self.height)-1)
        elif op == "L":
            for _ in range(n): self.rows.insert(self.y,[" "]*self.width); self.rows.pop(self.bottom+1)
        elif op == "M":
            for _ in range(n): self.rows.pop(self.y); self.rows.insert(self.bottom,[" "]*self.width)
    def feed(self, data):
        text = self.pending + self.decoder.decode(data)
        i = 0
        while i < len(text):
            c = text[i]
            if c == "\x1b":
                if i+1 == len(text): break
                op = text[i+1]
                if op == "[":
                    match = re.match(r"\x1b\[([0-9;? >]*)([@-~])", text[i:])
                    if not match: break
                    self.csi(*match.groups()); i += len(match.group()); continue
                if op in "()":
                    if i+2 == len(text): break
                    i += 3; continue
                if op == "7": self.saved = (self.y,self.x)
                elif op == "8": self.y,self.x = self.saved
                elif op == "M":
                    if self.y == self.top: self.rows.insert(self.top,[" "]*self.width); self.rows.pop(self.bottom+1)
                    else: self.y=max(0,self.y-1)
                i += 2; continue
            if c == "\r": self.x = 0
            elif c == "\n": self.newline()
            elif c == "\b": self.x = max(0,self.x-1)
            elif c == "\t": self.x = min(self.width-1, (self.x//8+1)*8)
            elif ord(c)>=32 and ord(c)!=127: self.char(c)
            i += 1
        self.pending = text[i:]
    @property
    def text(self):
        return "\n".join("".join(row) for row in self.rows)
