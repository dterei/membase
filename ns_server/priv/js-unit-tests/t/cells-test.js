
var CellsTest = TestCase("CellsTest");

CellsTest.prototype.setUp = function () {
  this.clockDestructor = Clock.hijack();
}

CellsTest.prototype.tearDown = function () {
  Clock.tickFarAway();
  this.clockDestructor();
  Cell.forgetState();
}

CellsTest.prototype.testSetup = function () {
  assert(!!this.clockDestructor);
}

CellsTest.prototype.testCallbacks = function () {
  var cell = new Cell();

  var anyFirings = 0;
  var definedFirings = 0;
  var undefinedFirings = 0;

  var assertIncreases = mkAssertIncreases(function () {
    return {anyFirings: anyFirings,
            definedFirings: definedFirings,
            undefinedFirings: undefinedFirings};
  });

  cell.subscribe(function () {
    definedFirings++;
  });

  cell.subscribeAny(function () {
    anyFirings++;
  });

  cell.subscribe(function () {
    undefinedFirings++;
  }, {
    'undefined': true,
    'changed': false
  });

  assertIncreases(["definedFirings", "anyFirings"],
                  function () {
                    cell.setValue(2);
                  });

  assertIncreases(["undefinedFirings", "anyFirings"],
                  function () {
                    cell.setValue(undefined);
                  });

  assertIncreases(["definedFirings", "anyFirings"],
                  function () {
                    cell.setValue(3);
                  });

  var deliverValue;

  assertIncreases(["undefinedFirings", "anyFirings"], function () {
    cell.setValue(future(function (_deliverValue) {
      deliverValue = _deliverValue;
    }))
  });

  assertEventuallyBecomes(function () {
    return !!deliverValue;
  });

  assertIncreases(["definedFirings", "anyFirings"],
                  function () {
                    deliverValue(4);
                  });

  assertEquals(4, cell.value);
}

CellsTest.prototype.checkFormulaWithFutureCase = function (sources) {
  var cell = new Cell(function () {
    if (realFormula)
      return realFormula();
  }, sources);

  var deliverValue;
  var realFormula = function () {
    return future(function (_deliverValue) {
      deliverValue = _deliverValue;
    });
  }

  assertSame(undefined, cell.value);

  cell.recalculate();

  assertEventuallyBecomes(function () {
    return !!deliverValue
  });

  assertSame(undefined, cell.value);

  deliverValue(3);

  assertEventuallyBecomes(function () {
    return cell.value == 3;
  });

  realFormula = function () {
    return 5;
  }

  cell.recalculate();

  assertEventuallyBecomes(function () {
    return cell.value == 5;
  });
}

CellsTest.prototype.testFormulaWithFuture = function () {
  this.checkFormulaWithFutureCase();
}

CellsTest.prototype.testFormulaWithFutureWithSources = function () {
  this.checkFormulaWithFutureCase({});
}

CellsTest.prototype.testFormulaCells = function () {
  var aCell = new Cell();
  var bCell = new Cell();
  var cCell = new Cell(function (a,b) {
    assertEquals(this.a, a);
    assertEquals(this.b, b);

    var context = this.self.context;
    assertEquals(context.a, aCell);
    assertEquals(aCell.value, a);
    assertEquals(context.b, bCell);
    assertEquals(bCell.value, b);

    return (a + b) % 3;
  }, {a:aCell,b:bCell});

  assertSame(undefined, cCell.value);

  var cUpdates = 0;

  cCell.subscribeAny(function () {cUpdates++});

  var assertIncreases = mkAssertIncreases(function () {
    return {cUpdates: cUpdates};
  });

  assertIncreases([], function () {
    aCell.setValue(1);
  });
  assertSame(undefined, cCell.value);

  assertIncreases("cUpdates", function () {
    bCell.setValue(0);
  });

  assertEquals(1, cCell.value);

  assertIncreases([], function () {
    bCell.setValue(3);
  });
  assertEquals(1, cCell.value);

  assertIncreases("cUpdates", function () {
    bCell.setValue(undefined);
    aCell.setValue(undefined);
  });

  assertSame(undefined, cCell.value);
}

// verifies that we don't overflow anything with long formula
// dependency chains and that we fire callbacks and initiate futures
// when everything is quiet
CellsTest.prototype.testLongChainsAndQuiscentState = function () {
  var cells = [new Cell()];
  _.each(_.range(1000), function (i) {
    cells.push(new Cell(function (dep) {
      if (i == cells.length/2)
        Clock.tickFarAway();
      return dep+1;
    }, {dep: cells[i]}))
  });

  var events = [];

  cells.push(new Cell(function (dep) {
    events.push("last-cell-calculated");
    return dep+1;
  }, {dep: cells[cells.length-1]}));

  cells[0].subscribeAny(function () {
    events.push("initial-cell-callback-fired");
  });

  var futureCell = new Cell(function (dep) {
    return future(function () {
      events.push("future-cell-async-started");
    });
  }, {dep: cells[cells.length/2]});

  cells[0].setValue(0);

  for (var i = cells.length*20; i >= 0; i--) {
    if (cells[cells.length-1].value !== undefined)
      break;
    Clock.tick(20);
  }

  assertEquals(cells.length-1, cells[cells.length-1].value);

  assertEventuallyBecomes(function () {
    return events.length == 3;
  }, function (raiser) {
    console.log("events:", events);
    raiser();
  });

  assertEquals("last-cell-calculated", events[0]);
  assertSetEquals(["future-cell-async-started", "initial-cell-callback-fired"],
                  events.slice(1));
}

CellsTest.prototype.testErrorDetection = function () {
  var cell = new Cell();
  var f = function () {};
  assertException(function () {
    new Cell(f, {c: cell, a: null});
  });
  assertException("non-cell source must be detected", function () {
    new Cell(f, {c: cell, a: 1});
  });
  var okCell;
  assertNoException(function () {
    okCell = new Cell(f, {c: cell});
  });

  assertException(function () {
    (new Cell()).setSources({c: cell});
  });
  assertException("sources cannot be updated yet", function () {
    okCell.setSources({a: new Cell()});
  });

  assertException("unknown sources in args must be detected", function () {
    new Cell(function (a,b) {
    }, {a: new Cell()});
  });
  assertNoException(function () {
    new Cell(function (a) {
    }, {a: new Cell()});
  });

  Clock.tickFarAway();
}

CellsTest.prototype.testSimpleFormulaCell = function () {
  var events = [];
  var cell = new Cell(function () {
    events.push(3);
    return 3;
  });

  Clock.tickFarAway();

  assertEquals([3], events)
  assertEquals(3, cell.value);
}

CellsTest.prototype.testInvalidate = function () {
  var events = [];
  var deliverValue;

  var cell = new Cell(function () {
    events.push("recalc");
    return future(function (_deliverValue) {
      deliverValue = _deliverValue;
    });
  });

  assertEventuallyBecomes(function () {
    return events.length == 1;
  });

  assertEventuallyBecomes(function () {
    return deliverValue != null;
  });

  assertEquals(["recalc"], events);

  cell.invalidate();
  Clock.tickFarAway();
  assertEquals("no pending future recalculation after invalidate",
               ["recalc"], events);

  deliverValue(1);
  Clock.tickFarAway();
  assertEquals(1, cell.value);

  assertEquals(["recalc"], events);
  cell.invalidate();
  Clock.tickFarAway();
  assertEquals("invalidate causes recalculation",
               ["recalc", "recalc"], events);
}

CellsTest.prototype.testDoubleFutureStartBug = function () {
  var cell = new Cell(function () {
    return future(function (deliver) {
      setTimeout(function () {
        deliver(3);
      }, 2000);
    });
  });
  var dependentCell = new Cell(function (a) {
    return future(function (deliver) {
      setTimeout(function () {
        deliver(a + 1);
      }, 500);
    });
  }, {a: cell});

  Clock.tickFarAway();

  cell.setValue(future(function () {}));
  Clock.tickFarAway();
  assertSame(undefined, cell.value);
  assert(!!cell.pendingFuture);

  cell.recalculate();
  Clock.tickFarAway();
  assertEquals(3, cell.value);
  assertEquals(4, dependentCell.value);
}


CellsTest.prototype.testCompute = function () {
  var cell1 = new Cell();
  cell1.setValue(1);
  var cell2 = new Cell()
  cell2.setValue(2);

  var cell3 = new Cell();
  cell3.setValue('cell1');

  cell1.name = 'cell1';
  cell2.name = 'cell2';
  cell3.name = 'cell3';

  var computations = 0;

  var switchedCell = Cell.compute(function (v) {
    computations++;
    switch (v(cell3)) {
    case 'cell1':
      return v(cell1);
    case 'cell2':
      return v(cell2);
    }
  });

  Clock.tickFarAway();

  //no demand no value
  assertSame(undefined, switchedCell.value);
  assertEquals(0, computations);

  var demand = switchedCell.subscribeValue(function () {});

  Clock.tickFarAway();

  assertEquals(1, switchedCell.value);
  assertEquals(2, computations);

  cell1.setValue('newCell1');
  Clock.tickFarAway()
  computations = 0;

  assertEquals('newCell1', switchedCell.value);

  cell2.setValue('newCell2');
  Clock.tickFarAway();

  assertEquals('newCell1', switchedCell.value);
  assertEquals(0, computations);

  cell3.setValue('unknown');
  Clock.tickFarAway();

  assertEquals(undefined, switchedCell.value);
  assertEquals(1, computations);

  cell1.setValue('newerCell1');
  Clock.tickFarAway();

  assertEquals(undefined, switchedCell.value);
  assertEquals(1, computations);

  cell3.setValue('cell2');
  Clock.tickFarAway();

  assertEquals('newCell2', switchedCell.value);
  assertEquals(2, computations);

  // nothing happens after we cancel demand
  demand.cancel();
  Clock.tickFarAway();

  assertEquals(2, computations);

  cell2.setValue('anotherValue');
  cell3.setValue('anotherValue');
  Clock.tickFarAway();

  assertEquals(2, computations);
}


CellsTest.prototype.testNeeding = function () {
  var cellA = new Cell();
  var cellB = new Cell();

  function mkGetter(cell) {return function () {return cell.value}}

  function withValue(cellC, body) {
    var subscription = cellC.subscribeValue(function () {});
    try {
      return body.call(null, mkGetter(cellC));
    } finally {
      subscription.cancel();
    }
  }

  function checkBody(getValue) {
    cellA.setValue();
    cellB.setValue();
    Clock.tickFarAway();
    assertEquals(undefined, getValue());

    cellA.setValue(1);
    Clock.tickFarAway();
    assertEquals(undefined, getValue());

    cellB.setValue(2);
    Clock.tickFarAway();
    assertEquals(3, getValue());
  }

  function checkAddLazy(cellC) {
    withValue(cellC,checkBody);
  }

  function checkAddEager(cellC) {
    checkBody(mkGetter(cellC));
  }

  checkAddLazy(Cell.needing(cellA, cellB).compute(function (v, a, b) {
    assertEquals(1, a);
    assertEquals(2, b);
    return a+b;
  }));

  checkAddLazy(Cell.needing(cellB, cellA).compute(function (v, b, a) {
    assertEquals(1, a);
    assertEquals(2, b);
    return a+b;
  }));

  checkAddEager(Cell.needing(cellA, cellB).computeEager(function (v, a, b) {
    assertEquals(1, a);
    assertEquals(2, b);
    return a+b;
  }));

  checkAddEager(Cell.needing(cellB, cellA).computeEager(function (v, b, a) {
    assertEquals(1, a);
    assertEquals(2, b);
    return a+b;
  }));

  checkAddLazy(Cell.compute(function (v) {
    var a = v.need(cellA);
    var b = v.need(cellB);
    assertEquals(1, a);
    assertEquals(2, b);
    return a+b;
  }));

  checkAddEager(Cell.computeEager(function (v) {
    var a = v.need(cellA);
    var b = v.need(cellB);
    assertEquals(1, a);
    assertEquals(2, b);
    return a+b;
  }));
}

CellsTest.prototype.testFuturesRestartingOnReattach = function () {
  var deliverValue;
  var futuresCount = 0;
  var cancelCount = 0;

  var initialValue = {};

  var cell = Cell.compute(function (v) {
    return future(function (cb) {
      cb.continuing(initialValue);

      futuresCount++;
      deliverValue = cb;
      cb.async.cancel = function () {
        cancelCount++;
      }
    });
  });

  Clock.tickFarAway();

  // no demand – no nothing
  assertEquals(undefined, cell.value);
  assertEquals(0, futuresCount);
  assertEquals(undefined, deliverValue);
  assert(!cell.pendingFuture);

  var demand;

  function setDemand() {
      demand = cell.subscribeValue(function () {});
  }

  // now add demand and observe that future has started
  setDemand();
  Clock.tickFarAway();

  assertEquals(initialValue, cell.value);
  assertEquals(1, futuresCount);
  assert(!!deliverValue);
  assert(!!cell.pendingFuture);
  assertEquals(0, cancelCount);

  // now cancel demand and observe that future is cancelled
  demand.cancel();
  Clock.tickFarAway();

  assertEquals(initialValue, cell.value);
  assertEquals(1, futuresCount);
  assertEquals(1, cancelCount);
  assert(!cell.pendingFuture);

  // now add demand back and observe that future is restarted
  setDemand();
  initialValue = {};
  deliverValue = null;
  Clock.tickFarAway();

  assertEquals(initialValue, cell.value);
  assertEquals(2, futuresCount);
  assertEquals(1, cancelCount);
  assert(!!deliverValue);
  assert(!!cell.pendingFuture);

  // now complete future, cancel demand and observe old value
  var newValue = {};
  deliverValue(newValue);
  assertEquals(newValue, cell.value);
  assertEquals(2, futuresCount);
  assertEquals(1, cancelCount);
  assert(!cell.pendingFuture);

  demand.cancel();
  Clock.tickFarAway();

  assertEquals(newValue, cell.value);
  assertEquals(2, futuresCount);
  assertEquals(1, cancelCount);
  assert(!cell.pendingFuture);

  // now add back demand and observe that future is _not_ started
  // (because it was not running when demand was cancelled)

  setDemand();
  Clock.tickFarAway();
  assertEquals(newValue, cell.value);
  assertEquals(2, futuresCount);
  assertEquals(1, cancelCount);
  assert(!cell.pendingFuture);
}

CellsTest.prototype.testFutureWrap = function () {
  var events = [];
  var wrapped1 = future.wrap(function (realCB, startInner) {
    events.push("1startInner");
    startInner(function (val) {
      events.push("1delivery");
      realCB(val + "+wrapped1");
    });
  });

  var wrapped2 = future.wrap(function (realCB, startInner) {
    events.push("2startInner");
    startInner(function (val) {
      events.push("2delivery");
      realCB(val + "+wrapped2");
    });
  }, wrapped1);

  var deliverValue;
  var cell = Cell.computeEager(function (v) {
    return wrapped2(function (cb) {
      events.push("actualStart");
      deliverValue = cb;
    }, {
      niceOption: true
    });
  });

  Clock.tickFarAway();
  assert(!!deliverValue);
  assert(!!cell.pendingFuture);
  assert(cell.pendingFuture.niceOption);

  // we see that real future is wrapped by wrapped1 which is wrapped by wrapped2
  // which actually starts real future body
  assertEquals(["1startInner", "2startInner", "actualStart"],
               events);

  events = [];

  deliverValue("value");

  // we observe that all wrappers may tamper with delivered value
  assertEquals("value+wrapped2+wrapped1", cell.value);

  // and we observe order of that tampering
  assertEquals(["2delivery", "1delivery"], events);

  // now we see that usual future works as usual
  events = [];
  wrapped2 = future;
  deliverValue = null;
  cell.recalculate();
  Clock.tickFarAway();
  assert(!!deliverValue);
  assertEquals(["actualStart"], events);

  deliverValue("new-value");
  assertEquals(["actualStart"], events);
  assertEquals("new-value", cell.value);
}

CellsTest.prototype.testCellSubscribeMV = function () {
  var cellA = new Cell();
  var cellB = new Cell();

  var bodyRunCounter = 0;
  var body = function (a, b) {
    assertEquals(undefined, a);
    assertEquals(undefined, b);
    bodyRunCounter++;
  }

  Cell.subscribeMultipleValues(function (a, b) {
    body(a,b);
  }, cellA, cellB);

  Clock.tickFarAway();
  assertEquals(1, bodyRunCounter);

  body = function (a, b) {
    assertEquals("a", a);
    assertEquals(undefined, b);
    bodyRunCounter++;
  }

  cellA.setValue("a");
  Clock.tickFarAway();
  assertEquals(2, bodyRunCounter);

  body = function (a,b) {
    assertEquals("a", a);
    assertEquals("b", b);
    bodyRunCounter++;
  }

  cellB.setValue("b");
  Clock.tickFarAway();
  assertEquals(3, bodyRunCounter);

  body =function (a,b) {
    assertEquals(undefined, a);
    assertEquals("b", b);
    bodyRunCounter++;
  }

  cellA.setValue();
  Clock.tickFarAway();
  assertEquals(4, bodyRunCounter);
}
