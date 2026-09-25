class Main {
  @:entryPoint
  static function main():Void {
    var n:Int = 3;
    trace("countdown");
    while (n > 0) {
      trace(n);
      n = n - 1;
    }
    if (n == 0) {
      trace("lift off");
    } else {
      trace("math broke");
    }
  }
}