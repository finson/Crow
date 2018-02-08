#include "DDMotor.h"

//---------------------------------------------------------------------------

extern DeviceTable *gDeviceTable;

/**
 * This device driver is for plain DC motors and assumes some sort of motor
 * control device is present.
 *
 * Information needed by the device driver about each attached motor is held in
 * an object of the LUMotor class.
 */
DDMotor::DDMotor(const char *dName, int lunCount) :
  DeviceDriver(dName, lunCount) {
  DEFINE_VERSION(0, 11, 0)

  motorCount = 0;
  if ((logicalUnitCount > 0) && (logicalUnitCount <= MAX_MOTORS)) {
    motorCount = constrain(logicalUnitCount,0,MAX_MOTORS);
  }
}

//---------------------------------------------------------------------------

int DDMotor::open(int opts, int flags, const char *name) {
  int lun;
  int status = DeviceDriver::open(opts, flags, name);   // find an unused lun
  if (status < 0) {
    return status;
  }

  lun = status;
  LUMotor *currentUnit = &servos[lun]; // get pointer to the corresponding Motor object

  currentUnit->detach();
  currentUnit->minPulse = MIN_PULSE_WIDTH;
  currentUnit->maxPulse = MAX_PULSE_WIDTH;

  logicalUnits[lun] = currentUnit;      // remember that this lun is in use
  return lun;
}

//---------------------------------------------------------------------------

int DDMotor::read(int handle, int flags, int reg, int count, byte *buf) {

  if (count < 0) return EINVAL;

  // First, handle registers that can be processed by the DeviceDriver base
  // class without knowing very much about our particular device type.

  int status = DeviceDriver::read(handle, flags, reg, count, buf);
  if (status != ENOTSUP) {
    return status;
  }

  int lun = getUnitNumber(handle);
  if (lun < 0 || lun >= logicalUnitCount) return EINVAL;
  LUMotor *currentUnit = static_cast<LUMotor *>(logicalUnits[lun]);
  if (currentUnit == 0) return ENOTCONN;

  //  Second, registers for which we must have a handle (ie, open() has
  //  been done) AND we need to be attached to a pin (and thus
  //  a servo)

  if (!(currentUnit->attached())) return ENODATA;

  switch (reg) {

  case (int)(REG::PIN):
    if (count < 2) return EMSGSIZE;
    fromHostTo16LE(currentUnit->pin, buf);
    return 2;

  case (int)(REG::RANGE_MICROSECONDS):
    if (count < 4) return EMSGSIZE;
    fromHostTo16LE(currentUnit->minPulse, buf);
    fromHostTo16LE(currentUnit->maxPulse, buf + 2);
    return 4;

  case (int)(REG::POSITION_DEGREES):
    if (count < 2) return EMSGSIZE;
    fromHostTo16LE(currentUnit->read(), buf);
    return 2;

  case (int)(REG::POSITION_MICROSECONDS):
    if (count < 2) return EMSGSIZE;
    fromHostTo16LE(currentUnit->readMicroseconds(), buf);
    return 2;

  default:
    return ENOTSUP;
  }
  return EPANIC;
}

//---------------------------------------------------------------------------

int DDMotor::write(int handle, int flags, int reg, int count, byte *buf) {
  int thePin;
  uint8_t channel;
  int loPulse;
  int hiPulse;
  int pos;
  bool isNewLock;

  // First, handle registers that can be processed by the DeviceDriver base
  // class without knowing very much about our particular device type.

  int status = DeviceDriver::write(handle, flags, reg, count, buf);
  if (status != ENOTSUP) {
    return status;
  }

  //  Second, handle registers that can only be processed if an open has been
  //  performed and there is an LUMotor object associated with the lun.

  int lun = getUnitNumber(handle);
  if (lun < 0 || lun >= logicalUnitCount) return EINVAL;
  LUMotor *currentUnit = static_cast<LUMotor *>(logicalUnits[lun]);
  if (currentUnit == 0) return ENOTCONN;

  switch (reg) {

  case (int)(REG::PIN):
    if (count != 2) return EMSGSIZE;
    thePin = from16LEToHost(buf);
    if (!(IS_PIN_PWM(thePin))) return EINVAL;

    if (currentUnit->attached()) return EBUSY;
    isNewLock = currentUnit->lockPin(thePin);
    if (!isNewLock) return EBUSY;

    channel = currentUnit->attach(thePin, currentUnit->minPulse, currentUnit->maxPulse);
    if (channel == 255) {
      currentUnit->unlockPin(thePin);
      return EMFILE;
    }
    currentUnit->pin = thePin;
    return 2;

  }

  //  Third, registers for which we must have a handle (ie, open() has
  //  been done) AND we need to be attached to a pin (and thus
  //  a servo)

  if (!currentUnit->attached()) return EREMOTEIO;

  // Enable continuous write, if requested

  if (flags == (int)DAF::MILLI_RUN) {
    currentUnit->pulseIncrement = (currentUnit->maxPulse-currentUnit->minPulse) / currentUnit->stepCount;
    currentUnit->currentStep = 0;
    DeviceDriver::milliRateRun((int)DAC::WRITE, handle, flags, reg, count,buf);
  } else if (flags == (int)DAF::MILLI_STOP) {
    DeviceDriver::milliRateStop((int)DAC::WRITE, handle, flags, reg, count,buf);
  }

  switch (reg) {

  case (int)(REG::RANGE_MICROSECONDS):
    if (count != 4) return EMSGSIZE;
    loPulse = from16LEToHost(buf);
    hiPulse = from16LEToHost(buf + 2);
    if (currentUnit->attached()) {
      currentUnit->detach();
      channel = currentUnit->attach(currentUnit->pin, loPulse, hiPulse);
      if (channel == 255) return EMFILE;
    }
    currentUnit->minPulse = loPulse;
    currentUnit->maxPulse = hiPulse;
    return 4;

  case (int)(REG::POSITION_DEGREES):
    if (count != 2) return EMSGSIZE;
    pos = from16LEToHost(buf);
    currentUnit->write(pos);
    return 2;

  case (int)(REG::POSITION_MICROSECONDS):
    if (count != 2) return EMSGSIZE;
    pos = from16LEToHost(buf);
    currentUnit->writeMicroseconds(pos);
    return 2;

  default:
    return ENOTSUP;
  }
  return EPANIC;
}

//---------------------------------------------------------------------------

int DDMotor::close(int handle, int flags) {
  int lun = getUnitNumber(handle);
  if (lun < 0 || lun >= logicalUnitCount) return EINVAL;
  LUMotor *currentUnit = static_cast<LUMotor *>(logicalUnits[lun]);
  if (currentUnit == 0) return ENOTCONN;

  if (currentUnit->attached()) {
    currentUnit->detach();
    currentUnit->unlockPin(currentUnit->pin);
  }
  logicalUnits[lun] = 0;
  return DeviceDriver::close(handle, flags);
}

//---------------------------------------------------------------------------

// If a continuous write has been requested, process it.

// One use of continuous write for this device is to have the servo follow a
// position profile.  The default, and currently the only, profile is a simple
// sweep between limits, but function tables or other interesting algorithms
// would be easy to implement for servos and other devices.

int DDMotor::processTimerEvent(int lun, int timerSelector, ClientReporter *report) {
  int nextStep;
  int nextPulse;
  int status;

  LUMotor *cU = static_cast<LUMotor *>(logicalUnits[getUnitNumber(lun)]);
  if (cU == 0) return ENOTCONN;

  int h = cU->eventAction[1].handle;
  int f = cU->eventAction[1].flags;
  int r = cU->eventAction[1].reg;
  int c = min(cU->eventAction[1].count,QUERY_BUFFER_SIZE);

  // Is it time to do another write?
  // If so, calculate new position and set it.

  if ((timerSelector == 1) && (cU->eventAction[1].enabled)) {
    if ((cU->eventAction[1].action) == (int)(DAC::WRITE))  {
      switch (cU->eventAction[1].reg) {
      case (int)(REG::POSITION_MICROSECONDS):
        nextStep = cU->currentStep + cU->stepIncrement;
        if ((nextStep < 0) || (nextStep == cU->stepCount)) {
          cU->stepIncrement = -cU->stepIncrement;
          nextStep = cU->currentStep + cU->stepIncrement;
        }
        cU->currentStep = nextStep;
        nextPulse = cU->minPulse + (nextStep * cU->pulseIncrement);
        fromHostTo16LE(nextPulse,cU->eventAction[1].queryBuffer);
        f = 0;
        c = 2;
        r = (int)(REG::POSITION_MICROSECONDS);
        status = gDeviceTable->write(h, f, r, c, cU->eventAction[1].queryBuffer);
        report->reportWrite(status, h, f, r, c);
        return status;
      }
    }
  }
  return ESUCCESS;
}
