/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsITelemetry.h"
#include "nsIVariant.h"
#include "nsVariant.h"
#include "nsHashKeys.h"
#include "nsBaseHashtable.h"
#include "nsClassHashtable.h"
#include "nsDataHashtable.h"
#include "nsIXPConnect.h"
#include "nsContentUtils.h"
#include "nsThreadUtils.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/Unused.h"

#include "TelemetryComms.h"
#include "TelemetryCommon.h"
#include "TelemetryIPCAccumulator.h"
#include "TelemetryScalar.h"
#include "TelemetryScalarData.h"

using mozilla::StaticMutex;
using mozilla::StaticMutexAutoLock;
using mozilla::Telemetry::Common::AutoHashtable;
using mozilla::Telemetry::Common::IsExpiredVersion;
using mozilla::Telemetry::Common::CanRecordDataset;
using mozilla::Telemetry::Common::IsInDataset;
using mozilla::Telemetry::Common::LogToBrowserConsole;
using mozilla::Telemetry::ScalarActionType;

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// Naming: there are two kinds of functions in this file:
//
// * Functions named internal_*: these can only be reached via an
//   interface function (TelemetryScalar::*). They expect the interface
//   function to have acquired |gTelemetryScalarsMutex|, so they do not
//   have to be thread-safe.
//
// * Functions named TelemetryScalar::*. This is the external interface.
//   Entries and exits to these functions are serialised using
//   |gTelemetryScalarsMutex|.
//
// Avoiding races and deadlocks:
//
// All functions in the external interface (TelemetryScalar::*) are
// serialised using the mutex |gTelemetryScalarsMutex|. This means
// that the external interface is thread-safe, and many of the
// internal_* functions can ignore thread safety. But it also brings
// a danger of deadlock if any function in the external interface can
// get back to that interface. That is, we will deadlock on any call
// chain like this
//
// TelemetryScalar::* -> .. any functions .. -> TelemetryScalar::*
//
// To reduce the danger of that happening, observe the following rules:
//
// * No function in TelemetryScalar::* may directly call, nor take the
//   address of, any other function in TelemetryScalar::*.
//
// * No internal function internal_* may call, nor take the address
//   of, any function in TelemetryScalar::*.

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// PRIVATE TYPES

namespace {

const uint32_t kMaximumNumberOfKeys = 100;
const uint32_t kMaximumKeyStringLength = 70;
const uint32_t kMaximumStringValueLength = 50;
const uint32_t kScalarCount =
  static_cast<uint32_t>(mozilla::Telemetry::ScalarID::ScalarCount);

enum class ScalarResult : uint8_t {
  // Nothing went wrong.
  Ok,
  // General Scalar Errors
  CannotRecordInProcess,
  OperationNotSupported,
  InvalidType,
  InvalidValue,
  // Keyed Scalar Errors
  KeyTooLong,
  TooManyKeys,
  // String Scalar Errors
  StringTooLong,
  // Unsigned Scalar Errors
  UnsignedNegativeValue,
  UnsignedTruncatedValue
};

typedef nsBaseHashtableET<nsDepCharHashKey, mozilla::Telemetry::ScalarID>
          CharPtrEntryType;

typedef AutoHashtable<CharPtrEntryType> ScalarMapType;

/**
 * Map the error codes used internally to NS_* error codes.
 * @param aSr The error code used internally in this module.
 * @return {nsresult} A NS_* error code.
 */
nsresult
MapToNsResult(ScalarResult aSr)
{
  switch (aSr) {
    case ScalarResult::Ok:
    case ScalarResult::CannotRecordInProcess:
      return NS_OK;
    case ScalarResult::OperationNotSupported:
      return NS_ERROR_NOT_AVAILABLE;
    case ScalarResult::StringTooLong:
      // We don't want to throw if we're setting a string that is too long.
      return NS_OK;
    case ScalarResult::InvalidType:
    case ScalarResult::InvalidValue:
    case ScalarResult::KeyTooLong:
      return NS_ERROR_ILLEGAL_VALUE;
    case ScalarResult::TooManyKeys:
      return NS_ERROR_FAILURE;
    case ScalarResult::UnsignedNegativeValue:
    case ScalarResult::UnsignedTruncatedValue:
      // We shouldn't throw if trying to set a negative number or are truncated,
      // only warn the user.
      return NS_OK;
  }
  return NS_ERROR_FAILURE;
}

bool
IsValidEnumId(mozilla::Telemetry::ScalarID aID)
{
  return aID < mozilla::Telemetry::ScalarID::ScalarCount;
}

/**
 * The following helpers are used to get a nsIVariant from a uint32_t,
 * nsAString or bool data.
 */
nsresult
GetVariant(uint32_t aValue, nsCOMPtr<nsIVariant>& aResult)
{
  nsCOMPtr<nsIWritableVariant> outVar(new nsVariant());
  nsresult rv = outVar->SetAsUint32(aValue);
  if (NS_FAILED(rv)) {
    return rv;
  }
  aResult = outVar.forget();
  return NS_OK;
}

nsresult
GetVariant(const nsAString& aValue, nsCOMPtr<nsIVariant>& aResult)
{
  nsCOMPtr<nsIWritableVariant> outVar(new nsVariant());
  nsresult rv = outVar->SetAsAString(aValue);
  if (NS_FAILED(rv)) {
    return rv;
  }
  aResult = outVar.forget();
  return NS_OK;
}

nsresult
GetVariant(bool aValue, nsCOMPtr<nsIVariant>& aResult)
{
  nsCOMPtr<nsIWritableVariant> outVar(new nsVariant());
  nsresult rv = outVar->SetAsBool(aValue);
  if (NS_FAILED(rv)) {
    return rv;
  }
  aResult = outVar.forget();
  return NS_OK;
}

// Implements the methods for ScalarInfo.
const char *
ScalarInfo::name() const
{
  return &gScalarsStringTable[this->name_offset];
}

const char *
ScalarInfo::expiration() const
{
  return &gScalarsStringTable[this->expiration_offset];
}

/**
 * The base scalar object, that servers as a common ancestor for storage
 * purposes.
 */
class ScalarBase
{
public:
  virtual ~ScalarBase() = default;

  // Set, Add and SetMaximum functions as described in the Telemetry IDL.
  virtual ScalarResult SetValue(nsIVariant* aValue) = 0;
  virtual ScalarResult AddValue(nsIVariant* aValue) { return ScalarResult::OperationNotSupported; }
  virtual ScalarResult SetMaximum(nsIVariant* aValue) { return ScalarResult::OperationNotSupported; }

  // Convenience methods used by the C++ API.
  virtual void SetValue(uint32_t aValue) { mozilla::Unused << HandleUnsupported(); }
  virtual ScalarResult SetValue(const nsAString& aValue) { return HandleUnsupported(); }
  virtual void SetValue(bool aValue) { mozilla::Unused << HandleUnsupported(); }
  virtual void AddValue(uint32_t aValue) { mozilla::Unused << HandleUnsupported(); }
  virtual void SetMaximum(uint32_t aValue) { mozilla::Unused << HandleUnsupported(); }

  // GetValue is used to get the value of the scalar when persisting it to JS.
  virtual nsresult GetValue(nsCOMPtr<nsIVariant>& aResult) const = 0;

  // To measure the memory stats.
  virtual size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const = 0;

private:
  ScalarResult HandleUnsupported() const;
};

ScalarResult
ScalarBase::HandleUnsupported() const
{
  MOZ_ASSERT(false, "This operation is not support for this scalar type.");
  return ScalarResult::OperationNotSupported;
}

/**
 * The implementation for the unsigned int scalar type.
 */
class ScalarUnsigned : public ScalarBase
{
public:
  using ScalarBase::SetValue;

  ScalarUnsigned() : mStorage(0) {};
  ~ScalarUnsigned() override = default;

  ScalarResult SetValue(nsIVariant* aValue) final;
  void SetValue(uint32_t aValue) final;
  ScalarResult AddValue(nsIVariant* aValue) final;
  void AddValue(uint32_t aValue) final;
  ScalarResult SetMaximum(nsIVariant* aValue) final;
  void SetMaximum(uint32_t aValue) final;
  nsresult GetValue(nsCOMPtr<nsIVariant>& aResult) const final;
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const final;

private:
  uint32_t mStorage;

  ScalarResult CheckInput(nsIVariant* aValue);

  // Prevent copying.
  ScalarUnsigned(const ScalarUnsigned& aOther) = delete;
  void operator=(const ScalarUnsigned& aOther) = delete;
};

ScalarResult
ScalarUnsigned::SetValue(nsIVariant* aValue)
{
  ScalarResult sr = CheckInput(aValue);
  if (sr == ScalarResult::UnsignedNegativeValue) {
    return sr;
  }

  if (NS_FAILED(aValue->GetAsUint32(&mStorage))) {
    return ScalarResult::InvalidValue;
  }
  return sr;
}

void
ScalarUnsigned::SetValue(uint32_t aValue)
{
  mStorage = aValue;
}

ScalarResult
ScalarUnsigned::AddValue(nsIVariant* aValue)
{
  ScalarResult sr = CheckInput(aValue);
  if (sr == ScalarResult::UnsignedNegativeValue) {
    return sr;
  }

  uint32_t newAddend = 0;
  nsresult rv = aValue->GetAsUint32(&newAddend);
  if (NS_FAILED(rv)) {
    return ScalarResult::InvalidValue;
  }
  mStorage += newAddend;
  return sr;
}

void
ScalarUnsigned::AddValue(uint32_t aValue)
{
  mStorage += aValue;
}

ScalarResult
ScalarUnsigned::SetMaximum(nsIVariant* aValue)
{
  ScalarResult sr = CheckInput(aValue);
  if (sr == ScalarResult::UnsignedNegativeValue) {
    return sr;
  }

  uint32_t newValue = 0;
  nsresult rv = aValue->GetAsUint32(&newValue);
  if (NS_FAILED(rv)) {
    return ScalarResult::InvalidValue;
  }
  if (newValue > mStorage) {
    mStorage = newValue;
  }
  return sr;
}

void
ScalarUnsigned::SetMaximum(uint32_t aValue)
{
  if (aValue > mStorage) {
    mStorage = aValue;
  }
}

nsresult
ScalarUnsigned::GetValue(nsCOMPtr<nsIVariant>& aResult) const
{
  return GetVariant(mStorage, aResult);
}

size_t
ScalarUnsigned::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  return aMallocSizeOf(this);
}

ScalarResult
ScalarUnsigned::CheckInput(nsIVariant* aValue)
{
  // If this is a floating point value/double, we will probably get truncated.
  uint16_t type;
  aValue->GetDataType(&type);
  if (type == nsIDataType::VTYPE_FLOAT ||
      type == nsIDataType::VTYPE_DOUBLE) {
    return ScalarResult::UnsignedTruncatedValue;
  }

  int32_t signedTest;
  // If we're able to cast the number to an int, check its sign.
  // Warn the user if he's trying to set the unsigned scalar to a negative
  // number.
  if (NS_SUCCEEDED(aValue->GetAsInt32(&signedTest)) &&
      signedTest < 0) {
    return ScalarResult::UnsignedNegativeValue;
  }
  return ScalarResult::Ok;
}

/**
 * The implementation for the string scalar type.
 */
class ScalarString : public ScalarBase
{
public:
  using ScalarBase::SetValue;

  ScalarString() : mStorage(EmptyString()) {};
  ~ScalarString() override = default;

  ScalarResult SetValue(nsIVariant* aValue) final;
  ScalarResult SetValue(const nsAString& aValue) final;
  nsresult GetValue(nsCOMPtr<nsIVariant>& aResult) const final;
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const final;

private:
  nsString mStorage;

  // Prevent copying.
  ScalarString(const ScalarString& aOther) = delete;
  void operator=(const ScalarString& aOther) = delete;
};

ScalarResult
ScalarString::SetValue(nsIVariant* aValue)
{
  // Check that we got the correct data type.
  uint16_t type;
  aValue->GetDataType(&type);
  if (type != nsIDataType::VTYPE_CHAR &&
      type != nsIDataType::VTYPE_WCHAR &&
      type != nsIDataType::VTYPE_DOMSTRING &&
      type != nsIDataType::VTYPE_CHAR_STR &&
      type != nsIDataType::VTYPE_WCHAR_STR &&
      type != nsIDataType::VTYPE_STRING_SIZE_IS &&
      type != nsIDataType::VTYPE_WSTRING_SIZE_IS &&
      type != nsIDataType::VTYPE_UTF8STRING &&
      type != nsIDataType::VTYPE_CSTRING &&
      type != nsIDataType::VTYPE_ASTRING) {
    return ScalarResult::InvalidType;
  }

  nsAutoString convertedString;
  nsresult rv = aValue->GetAsAString(convertedString);
  if (NS_FAILED(rv)) {
    return ScalarResult::InvalidValue;
  }
  return SetValue(convertedString);
};

ScalarResult
ScalarString::SetValue(const nsAString& aValue)
{
  mStorage = Substring(aValue, 0, kMaximumStringValueLength);
  if (aValue.Length() > kMaximumStringValueLength) {
    return ScalarResult::StringTooLong;
  }
  return ScalarResult::Ok;
}

nsresult
ScalarString::GetValue(nsCOMPtr<nsIVariant>& aResult) const
{
  return GetVariant(mStorage, aResult);
}

size_t
ScalarString::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  size_t n = aMallocSizeOf(this);
  n+= mStorage.SizeOfExcludingThisIfUnshared(aMallocSizeOf);
  return n;
}

/**
 * The implementation for the boolean scalar type.
 */
class ScalarBoolean : public ScalarBase
{
public:
  using ScalarBase::SetValue;

  ScalarBoolean() : mStorage(false) {};
  ~ScalarBoolean() override = default;

  ScalarResult SetValue(nsIVariant* aValue) final;
  void SetValue(bool aValue) final;
  nsresult GetValue(nsCOMPtr<nsIVariant>& aResult) const final;
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const final;

private:
  bool mStorage;

  // Prevent copying.
  ScalarBoolean(const ScalarBoolean& aOther) = delete;
  void operator=(const ScalarBoolean& aOther) = delete;
};

ScalarResult
ScalarBoolean::SetValue(nsIVariant* aValue)
{
  // Check that we got the correct data type.
  uint16_t type;
  aValue->GetDataType(&type);
  if (type != nsIDataType::VTYPE_BOOL &&
      type != nsIDataType::VTYPE_INT8 &&
      type != nsIDataType::VTYPE_INT16 &&
      type != nsIDataType::VTYPE_INT32 &&
      type != nsIDataType::VTYPE_INT64 &&
      type != nsIDataType::VTYPE_UINT8 &&
      type != nsIDataType::VTYPE_UINT16 &&
      type != nsIDataType::VTYPE_UINT32 &&
      type != nsIDataType::VTYPE_UINT64) {
    return ScalarResult::InvalidType;
  }

  if (NS_FAILED(aValue->GetAsBool(&mStorage))) {
    return ScalarResult::InvalidValue;
  }
  return ScalarResult::Ok;
};

void
ScalarBoolean::SetValue(bool aValue)
{
  mStorage = aValue;
}

nsresult
ScalarBoolean::GetValue(nsCOMPtr<nsIVariant>& aResult) const
{
  return GetVariant(mStorage, aResult);
}

size_t
ScalarBoolean::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
{
  return aMallocSizeOf(this);
}

/**
 * Allocate a scalar class given the scalar info.
 *
 * @param aInfo The informations for the scalar coming from the definition file.
 * @return nullptr if the scalar type is unknown, otherwise a valid pointer to the
 *         scalar type.
 */
ScalarBase*
internal_ScalarAllocate(uint32_t aScalarKind)
{
  ScalarBase* scalar = nullptr;
  switch (aScalarKind) {
  case nsITelemetry::SCALAR_COUNT:
    scalar = new ScalarUnsigned();
    break;
  case nsITelemetry::SCALAR_STRING:
    scalar = new ScalarString();
    break;
  case nsITelemetry::SCALAR_BOOLEAN:
    scalar = new ScalarBoolean();
    break;
  default:
    MOZ_ASSERT(false, "Invalid scalar type");
  }
  return scalar;
}

/**
 * The implementation for the keyed scalar type.
 */
class KeyedScalar
{
public:
  typedef mozilla::Pair<nsCString, nsCOMPtr<nsIVariant>> KeyValuePair;

  explicit KeyedScalar(uint32_t aScalarKind) : mScalarKind(aScalarKind) {};
  ~KeyedScalar() = default;

  // Set, Add and SetMaximum functions as described in the Telemetry IDL.
  // These methods implicitly instantiate a Scalar[*] for each key.
  ScalarResult SetValue(const nsAString& aKey, nsIVariant* aValue);
  ScalarResult AddValue(const nsAString& aKey, nsIVariant* aValue);
  ScalarResult SetMaximum(const nsAString& aKey, nsIVariant* aValue);

  // Convenience methods used by the C++ API.
  void SetValue(const nsAString& aKey, uint32_t aValue);
  void SetValue(const nsAString& aKey, bool aValue);
  void AddValue(const nsAString& aKey, uint32_t aValue);
  void SetMaximum(const nsAString& aKey, uint32_t aValue);

  // GetValue is used to get the key-value pairs stored in the keyed scalar
  // when persisting it to JS.
  nsresult GetValue(nsTArray<KeyValuePair>& aValues) const;

  // To measure the memory stats.
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf);

private:
  typedef nsClassHashtable<nsCStringHashKey, ScalarBase> ScalarKeysMapType;

  ScalarKeysMapType mScalarKeys;
  const uint32_t mScalarKind;

  ScalarResult GetScalarForKey(const nsAString& aKey, ScalarBase** aRet);
};

ScalarResult
KeyedScalar::SetValue(const nsAString& aKey, nsIVariant* aValue)
{
  ScalarBase* scalar = nullptr;
  ScalarResult sr = GetScalarForKey(aKey, &scalar);
  if (sr != ScalarResult::Ok) {
    return sr;
  }

  return scalar->SetValue(aValue);
}

ScalarResult
KeyedScalar::AddValue(const nsAString& aKey, nsIVariant* aValue)
{
  ScalarBase* scalar = nullptr;
  ScalarResult sr = GetScalarForKey(aKey, &scalar);
  if (sr != ScalarResult::Ok) {
    return sr;
  }

  return scalar->AddValue(aValue);
}

ScalarResult
KeyedScalar::SetMaximum(const nsAString& aKey, nsIVariant* aValue)
{
  ScalarBase* scalar = nullptr;
  ScalarResult sr = GetScalarForKey(aKey, &scalar);
  if (sr != ScalarResult::Ok) {
    return sr;
  }

  return scalar->SetMaximum(aValue);
}

void
KeyedScalar::SetValue(const nsAString& aKey, uint32_t aValue)
{
  ScalarBase* scalar = nullptr;
  ScalarResult sr = GetScalarForKey(aKey, &scalar);
  if (sr != ScalarResult::Ok) {
    MOZ_ASSERT(false, "Key too long or too many keys are recorded in the scalar.");
    return;
  }

  return scalar->SetValue(aValue);
}

void
KeyedScalar::SetValue(const nsAString& aKey, bool aValue)
{
  ScalarBase* scalar = nullptr;
  ScalarResult sr = GetScalarForKey(aKey, &scalar);
  if (sr != ScalarResult::Ok) {
    MOZ_ASSERT(false, "Key too long or too many keys are recorded in the scalar.");
    return;
  }

  return scalar->SetValue(aValue);
}

void
KeyedScalar::AddValue(const nsAString& aKey, uint32_t aValue)
{
  ScalarBase* scalar = nullptr;
  ScalarResult sr = GetScalarForKey(aKey, &scalar);
  if (sr != ScalarResult::Ok) {
    MOZ_ASSERT(false, "Key too long or too many keys are recorded in the scalar.");
    return;
  }

  return scalar->AddValue(aValue);
}

void
KeyedScalar::SetMaximum(const nsAString& aKey, uint32_t aValue)
{
  ScalarBase* scalar = nullptr;
  ScalarResult sr = GetScalarForKey(aKey, &scalar);
  if (sr != ScalarResult::Ok) {
    MOZ_ASSERT(false, "Key too long or too many keys are recorded in the scalar.");
    return;
  }

  return scalar->SetMaximum(aValue);
}

/**
 * Get a key-value array with the values for the Keyed Scalar.
 * @param aValue The array that will hold the key-value pairs.
 * @return {nsresult} NS_OK or an error value as reported by the
 *         the specific scalar objects implementations (e.g.
 *         ScalarUnsigned).
 */
nsresult
KeyedScalar::GetValue(nsTArray<KeyValuePair>& aValues) const
{
  for (auto iter = mScalarKeys.ConstIter(); !iter.Done(); iter.Next()) {
    ScalarBase* scalar = static_cast<ScalarBase*>(iter.Data());

    // Get the scalar value.
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = scalar->GetValue(scalarValue);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // Append it to value list.
    aValues.AppendElement(mozilla::MakePair(nsCString(iter.Key()), scalarValue));
  }

  return NS_OK;
}

/**
 * Get the scalar for the referenced key.
 * If there's no such key, instantiate a new Scalar object with the
 * same type of the Keyed scalar and create the key.
 */
ScalarResult
KeyedScalar::GetScalarForKey(const nsAString& aKey, ScalarBase** aRet)
{
  if (aKey.Length() >= kMaximumKeyStringLength) {
    return ScalarResult::KeyTooLong;
  }

  if (mScalarKeys.Count() >= kMaximumNumberOfKeys) {
    return ScalarResult::TooManyKeys;
  }

  NS_ConvertUTF16toUTF8 utf8Key(aKey);

  ScalarBase* scalar = nullptr;
  if (mScalarKeys.Get(utf8Key, &scalar)) {
    *aRet = scalar;
    return ScalarResult::Ok;
  }

  scalar = internal_ScalarAllocate(mScalarKind);
  if (!scalar) {
    return ScalarResult::InvalidType;
  }

  mScalarKeys.Put(utf8Key, scalar);

  *aRet = scalar;
  return ScalarResult::Ok;
}

size_t
KeyedScalar::SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf)
{
  size_t n = aMallocSizeOf(this);
  for (auto iter = mScalarKeys.Iter(); !iter.Done(); iter.Next()) {
    ScalarBase* scalar = static_cast<ScalarBase*>(iter.Data());
    n += scalar->SizeOfIncludingThis(aMallocSizeOf);
  }
  return n;
}

typedef nsUint32HashKey ScalarIDHashKey;
typedef nsUint32HashKey ProcessIDHashKey;
typedef nsClassHashtable<ScalarIDHashKey, ScalarBase> ScalarStorageMapType;
typedef nsClassHashtable<ScalarIDHashKey, KeyedScalar> KeyedScalarStorageMapType;
typedef nsClassHashtable<ProcessIDHashKey, ScalarStorageMapType> ProcessesScalarsMapType;
typedef nsClassHashtable<ProcessIDHashKey, KeyedScalarStorageMapType> ProcessesKeyedScalarsMapType;

} // namespace

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// PRIVATE STATE, SHARED BY ALL THREADS

namespace {

// Set to true once this global state has been initialized.
bool gInitDone = false;

bool gCanRecordBase;
bool gCanRecordExtended;

// The Name -> ID cache map.
ScalarMapType gScalarNameIDMap(kScalarCount);

// The (Process Id -> (Scalar ID -> Scalar Object)) map. This is a nsClassHashtable,
// it owns the scalar instances and takes care of deallocating them when they are
// removed from the map.
ProcessesScalarsMapType gScalarStorageMap;
// As above, for the keyed scalars.
ProcessesKeyedScalarsMapType gKeyedScalarStorageMap;

} // namespace

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// PRIVATE: Function that may call JS code.

// NOTE: the functions in this section all run without protection from
// |gTelemetryScalarsMutex|. If they held the mutex, there would be the
// possibility of deadlock because the JS_ calls that they make may call
// back into the TelemetryScalar interface, hence trying to re-acquire the mutex.
//
// This means that these functions potentially race against threads, but
// that seems preferable to risking deadlock.

namespace {

/**
 * Checks if the error should be logged.
 *
 * @param aSr The error code.
 * @return true if the error should be logged, false otherwise.
 */
bool
internal_ShouldLogError(ScalarResult aSr)
{
  switch (aSr) {
    case ScalarResult::CannotRecordInProcess: MOZ_FALLTHROUGH;
    case ScalarResult::StringTooLong: MOZ_FALLTHROUGH;
    case ScalarResult::KeyTooLong: MOZ_FALLTHROUGH;
    case ScalarResult::TooManyKeys: MOZ_FALLTHROUGH;
    case ScalarResult::UnsignedNegativeValue: MOZ_FALLTHROUGH;
    case ScalarResult::UnsignedTruncatedValue:
      // Intentional fall-through.
      return true;

    default:
      return false;
  }

  // It should never reach this point.
  return false;
}

/**
 * Converts the error code to a human readable error message and prints it to the
 * browser console.
 *
 * @param aScalarName The name of the scalar that raised the error.
 * @param aSr The error code.
 */
void
internal_LogScalarError(const nsACString& aScalarName, ScalarResult aSr)
{
  nsAutoString errorMessage;
  AppendUTF8toUTF16(aScalarName, errorMessage);

  switch (aSr) {
    case ScalarResult::CannotRecordInProcess:
      errorMessage.Append(NS_LITERAL_STRING(" - Cannot record the scalar in the current process."));
      break;
    case ScalarResult::StringTooLong:
      errorMessage.Append(NS_LITERAL_STRING(" - Truncating scalar value to 50 characters."));
      break;
    case ScalarResult::KeyTooLong:
      errorMessage.Append(NS_LITERAL_STRING(" - The key length must be limited to 70 characters."));
      break;
    case ScalarResult::TooManyKeys:
      errorMessage.Append(NS_LITERAL_STRING(" - Keyed scalars cannot have more than 100 keys."));
      break;
    case ScalarResult::UnsignedNegativeValue:
      errorMessage.Append(NS_LITERAL_STRING(" - Trying to set an unsigned scalar to a negative number."));
      break;
    case ScalarResult::UnsignedTruncatedValue:
      errorMessage.Append(NS_LITERAL_STRING(" - Truncating float/double number."));
      break;
    default:
      // Nothing.
      return;
  }

  LogToBrowserConsole(nsIScriptError::warningFlag, errorMessage);
}

} // namespace

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// PRIVATE: thread-unsafe helpers for the external interface

namespace {

bool
internal_CanRecordBase()
{
  return gCanRecordBase;
}

bool
internal_CanRecordExtended()
{
  return gCanRecordExtended;
}

const ScalarInfo&
internal_InfoForScalarID(mozilla::Telemetry::ScalarID aId)
{
  return gScalars[static_cast<uint32_t>(aId)];
}

/**
 * Check if the given scalar is a keyed scalar.
 *
 * @param aId The scalar enum.
 * @return true if aId refers to a keyed scalar, false otherwise.
 */
bool
internal_IsKeyedScalar(mozilla::Telemetry::ScalarID aId)
{
  return internal_InfoForScalarID(aId).keyed;
}

/**
 * Check if we're allowed to record the given scalar in the current
 * process.
 *
 * @param aId The id of the scalar to check.
 * @return true if the scalar is allowed to be recorded in the current process, false
 *         otherwise.
 */
bool
internal_CanRecordProcess(mozilla::Telemetry::ScalarID aId)
{
  // Get the scalar info from the id.
  const ScalarInfo &info = internal_InfoForScalarID(aId);

  bool recordAllChild = !!(info.record_in_processes & RecordedProcessType::AllChilds);
  // We can use (1 << ProcessType) due to the way RecordedProcessType is defined
  // in ScalarInfo.h
  bool canRecordProcess =
    !!(info.record_in_processes & static_cast<RecordedProcessType>(1 << XRE_GetProcessType()));

  return canRecordProcess || (!XRE_IsParentProcess() && recordAllChild);
}

bool
internal_CanRecordForScalarID(mozilla::Telemetry::ScalarID aId)
{
  // Get the scalar info from the id.
  const ScalarInfo &info = internal_InfoForScalarID(aId);

  // Can we record at all?
  bool canRecordBase = internal_CanRecordBase();
  if (!canRecordBase) {
    return false;
  }

  bool canRecordDataset = CanRecordDataset(info.dataset,
                                           canRecordBase,
                                           internal_CanRecordExtended());
  if (!canRecordDataset) {
    return false;
  }

  return true;
}

/**
 * Get the scalar enum id from the scalar name.
 *
 * @param aName The scalar name.
 * @param aId The output variable to contain the enum.
 * @return
 *   NS_ERROR_FAILURE if this was called before init is completed.
 *   NS_ERROR_INVALID_ARG if the name can't be found in the scalar definitions.
 *   NS_OK if the scalar was found and aId contains a valid enum id.
 */
nsresult
internal_GetEnumByScalarName(const nsACString& aName, mozilla::Telemetry::ScalarID* aId)
{
  if (!gInitDone) {
    return NS_ERROR_FAILURE;
  }

  CharPtrEntryType *entry = gScalarNameIDMap.GetEntry(PromiseFlatCString(aName).get());
  if (!entry) {
    return NS_ERROR_INVALID_ARG;
  }
  *aId = entry->mData;
  return NS_OK;
}

/**
 * Get a scalar object by its enum id. This implicitly allocates the scalar
 * object in the storage if it wasn't previously allocated.
 *
 * @param aId The scalar id.
 * @param aProcessStorage This drives the selection of the map to use to store
 *        the scalar data coming from child processes. This is only meaningful when
 *        this function is called in parent process. If that's the case, if
 *        this is not |GeckoProcessType_Default|, the process id is used to
 *        allocate and store the scalars.
 * @param aRes The output variable that stores scalar object.
 * @return
 *   NS_ERROR_INVALID_ARG if the scalar id is unknown.
 *   NS_ERROR_NOT_AVAILABLE if the scalar is expired.
 *   NS_OK if the scalar was found. If that's the case, aResult contains a
 *   valid pointer to a scalar type.
 */
nsresult
internal_GetScalarByEnum(mozilla::Telemetry::ScalarID aId, GeckoProcessType aProcessStorage,
                         ScalarBase** aRet)
{
  if (!IsValidEnumId(aId)) {
    MOZ_ASSERT(false, "Requested a scalar with an invalid id.");
    return NS_ERROR_INVALID_ARG;
  }

  const uint32_t id = static_cast<uint32_t>(aId);
  const ScalarInfo &info = gScalars[id];

  ScalarBase* scalar = nullptr;
  ScalarStorageMapType* scalarStorage = nullptr;
  // Initialize the scalar storage to the parent storage. This will get
  // set to the child storage if needed.
  uint32_t storageId = static_cast<uint32_t>(aProcessStorage);

  // Get the process-specific storage or create one if it's not
  // available.
  if (!gScalarStorageMap.Get(storageId, &scalarStorage)) {
    scalarStorage = new ScalarStorageMapType();
    gScalarStorageMap.Put(storageId, scalarStorage);
  }

  // Check if the scalar is already allocated in the parent or in the child storage.
  if (scalarStorage->Get(id, &scalar)) {
    *aRet = scalar;
    return NS_OK;
  }

  if (IsExpiredVersion(info.expiration())) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  scalar = internal_ScalarAllocate(info.kind);
  if (!scalar) {
    return NS_ERROR_INVALID_ARG;
  }

  scalarStorage->Put(id, scalar);
  *aRet = scalar;
  return NS_OK;
}

/**
 * Get a scalar object by its enum id, if we're allowed to record it.
 *
 * @param aId The scalar id.
 * @return The ScalarBase instance or nullptr if we're not allowed to record
 *         the scalar.
 */
ScalarBase*
internal_GetRecordableScalar(mozilla::Telemetry::ScalarID aId)
{
  // Get the scalar by the enum (it also internally checks for aId validity).
  ScalarBase* scalar = nullptr;
  nsresult rv = internal_GetScalarByEnum(aId, GeckoProcessType_Default, &scalar);
  if (NS_FAILED(rv)) {
    return nullptr;
  }

  if (internal_IsKeyedScalar(aId)) {
    return nullptr;
  }

  // Are we allowed to record this scalar?
  if (!internal_CanRecordForScalarID(aId) || !internal_CanRecordProcess(aId)) {
    return nullptr;
  }

  return scalar;
}

} // namespace



////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// PRIVATE: thread-unsafe helpers for the keyed scalars

namespace {

/**
 * Get a keyed scalar object by its enum id. This implicitly allocates the keyed
 * scalar object in the storage if it wasn't previously allocated.
 *
 * @param aId The scalar id.
 * @param aProcessStorage This drives the selection of the map to use to store
 *        the scalar data coming from child processes. This is only meaningful when
 *        this function is called in parent process. If that's the case, if
 *        this is not |GeckoProcessType_Default|, the process id is used to
 *        allocate and store the scalars.
 * @param aRet The output variable that stores scalar object.
 * @return
 *   NS_ERROR_INVALID_ARG if the scalar id is unknown or a this is a keyed string
 *                        scalar.
 *   NS_ERROR_NOT_AVAILABLE if the scalar is expired.
 *   NS_OK if the scalar was found. If that's the case, aResult contains a
 *   valid pointer to a scalar type.
 */
nsresult
internal_GetKeyedScalarByEnum(mozilla::Telemetry::ScalarID aId, GeckoProcessType aProcessStorage,
                              KeyedScalar** aRet)
{
  if (!IsValidEnumId(aId)) {
    MOZ_ASSERT(false, "Requested a keyed scalar with an invalid id.");
    return NS_ERROR_INVALID_ARG;
  }

  const uint32_t id = static_cast<uint32_t>(aId);
  const ScalarInfo &info = gScalars[id];

  KeyedScalar* scalar = nullptr;
  KeyedScalarStorageMapType* scalarStorage = nullptr;
  // Initialize the scalar storage to the parent storage. This will get
  // set to the child storage if needed.
  uint32_t storageId = static_cast<uint32_t>(aProcessStorage);

  // Get the process-specific storage or create one if it's not
  // available.
  if (!gKeyedScalarStorageMap.Get(storageId, &scalarStorage)) {
    scalarStorage = new KeyedScalarStorageMapType();
    gKeyedScalarStorageMap.Put(storageId, scalarStorage);
  }

  if (scalarStorage->Get(id, &scalar)) {
    *aRet = scalar;
    return NS_OK;
  }

  if (IsExpiredVersion(info.expiration())) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // We don't currently support keyed string scalars. Disable them.
  if (info.kind == nsITelemetry::SCALAR_STRING) {
    MOZ_ASSERT(false, "Keyed string scalars are not currently supported.");
    return NS_ERROR_INVALID_ARG;
  }

  scalar = new KeyedScalar(info.kind);
  if (!scalar) {
    return NS_ERROR_INVALID_ARG;
  }

  scalarStorage->Put(id, scalar);
  *aRet = scalar;
  return NS_OK;
}

/**
 * Get a keyed scalar object by its enum id, if we're allowed to record it.
 *
 * @param aId The scalar id.
 * @return The KeyedScalar instance or nullptr if we're not allowed to record
 *         the scalar.
 */
KeyedScalar*
internal_GetRecordableKeyedScalar(mozilla::Telemetry::ScalarID aId)
{
  // Get the scalar by the enum (it also internally checks for aId validity).
  KeyedScalar* scalar = nullptr;
  nsresult rv = internal_GetKeyedScalarByEnum(aId, GeckoProcessType_Default, &scalar);
  if (NS_FAILED(rv)) {
    return nullptr;
  }

  if (!internal_IsKeyedScalar(aId)) {
    return nullptr;
  }

  // Are we allowed to record this scalar?
  if (!internal_CanRecordForScalarID(aId) || !internal_CanRecordProcess(aId)) {
    return nullptr;
  }

  return scalar;
}

} // namespace

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// EXTERNALLY VISIBLE FUNCTIONS in namespace TelemetryScalars::

// This is a StaticMutex rather than a plain Mutex (1) so that
// it gets initialised in a thread-safe manner the first time
// it is used, and (2) because it is never de-initialised, and
// a normal Mutex would show up as a leak in BloatView.  StaticMutex
// also has the "OffTheBooks" property, so it won't show as a leak
// in BloatView.
// Another reason to use a StaticMutex instead of a plain Mutex is
// that, due to the nature of Telemetry, we cannot rely on having a
// mutex initialized in InitializeGlobalState. Unfortunately, we
// cannot make sure that no other function is called before this point.
static StaticMutex gTelemetryScalarsMutex;

void
TelemetryScalar::InitializeGlobalState(bool aCanRecordBase, bool aCanRecordExtended)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  MOZ_ASSERT(!gInitDone, "TelemetryScalar::InitializeGlobalState "
             "may only be called once");

  gCanRecordBase = aCanRecordBase;
  gCanRecordExtended = aCanRecordExtended;

  // Populate the static scalar name->id cache. Note that the scalar names are
  // statically allocated and come from the automatically generated TelemetryScalarData.h.
  uint32_t scalarCount = static_cast<uint32_t>(mozilla::Telemetry::ScalarID::ScalarCount);
  for (uint32_t i = 0; i < scalarCount; i++) {
    CharPtrEntryType *entry = gScalarNameIDMap.PutEntry(gScalars[i].name());
    entry->mData = static_cast<mozilla::Telemetry::ScalarID>(i);
  }

#ifdef DEBUG
  gScalarNameIDMap.MarkImmutable();
#endif
  gInitDone = true;
}

void
TelemetryScalar::DeInitializeGlobalState()
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  gCanRecordBase = false;
  gCanRecordExtended = false;
  gScalarNameIDMap.Clear();
  gScalarStorageMap.Clear();
  gKeyedScalarStorageMap.Clear();
  gInitDone = false;
}

void
TelemetryScalar::SetCanRecordBase(bool b)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  gCanRecordBase = b;
}

void
TelemetryScalar::SetCanRecordExtended(bool b) {
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  gCanRecordExtended = b;
}

/**
 * Adds the value to the given scalar.
 *
 * @param aName The scalar name.
 * @param aVal The numeric value to add to the scalar.
 * @param aCx The JS context.
 * @return NS_OK if the value was added or if we're not allowed to record to this
 *  dataset. Otherwise, return an error.
 */
nsresult
TelemetryScalar::Add(const nsACString& aName, JS::HandleValue aVal, JSContext* aCx)
{
  // Unpack the aVal to nsIVariant. This uses the JS context.
  nsCOMPtr<nsIVariant> unpackedVal;
  nsresult rv =
    nsContentUtils::XPConnect()->JSToVariant(aCx, aVal,  getter_AddRefs(unpackedVal));
  if (NS_FAILED(rv)) {
    return rv;
  }

  ScalarResult sr;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);

    mozilla::Telemetry::ScalarID id;
    rv = internal_GetEnumByScalarName(aName, &id);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // We're trying to set a plain scalar, so make sure this is one.
    if (internal_IsKeyedScalar(id)) {
      return NS_ERROR_ILLEGAL_VALUE;
    }

    // Are we allowed to record this scalar?
    if (!internal_CanRecordForScalarID(id)) {
      return NS_OK;
    }

    if (internal_CanRecordProcess(id)) {
      // Accumulate in the child process if needed.
      if (!XRE_IsParentProcess()) {
        const ScalarInfo &info = gScalars[static_cast<uint32_t>(id)];
        TelemetryIPCAccumulator::RecordChildScalarAction(id, info.kind, ScalarActionType::eAdd,
                                                         unpackedVal);
        return NS_OK;
      }

      // Finally get the scalar.
      ScalarBase* scalar = nullptr;
      rv = internal_GetScalarByEnum(id, GeckoProcessType_Default, &scalar);
      if (NS_FAILED(rv)) {
        // Don't throw on expired scalars.
        if (rv == NS_ERROR_NOT_AVAILABLE) {
          return NS_OK;
        }
        return rv;
      }

      sr = scalar->AddValue(unpackedVal);
    } else {
      sr = ScalarResult::CannotRecordInProcess;
    }
  }

  // Warn the user about the error if we need to.
  if (internal_ShouldLogError(sr)) {
    internal_LogScalarError(aName, sr);
  }

  return MapToNsResult(sr);
}

/**
 * Adds the value to the given scalar.
 *
 * @param aName The scalar name.
 * @param aKey The key name.
 * @param aVal The numeric value to add to the scalar.
 * @param aCx The JS context.
 * @return NS_OK if the value was added or if we're not allow to record to this
 *  dataset. Otherwise, return an error.
 */
nsresult
TelemetryScalar::Add(const nsACString& aName, const nsAString& aKey, JS::HandleValue aVal,
                     JSContext* aCx)
{
  // Unpack the aVal to nsIVariant. This uses the JS context.
  nsCOMPtr<nsIVariant> unpackedVal;
  nsresult rv =
    nsContentUtils::XPConnect()->JSToVariant(aCx, aVal,  getter_AddRefs(unpackedVal));
  if (NS_FAILED(rv)) {
    return rv;
  }

  ScalarResult sr;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);

    mozilla::Telemetry::ScalarID id;
    rv = internal_GetEnumByScalarName(aName, &id);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // Make sure this is a keyed scalar.
    if (!internal_IsKeyedScalar(id)) {
      return NS_ERROR_ILLEGAL_VALUE;
    }

    // Are we allowed to record this scalar?
    if (!internal_CanRecordForScalarID(id)) {
      return NS_OK;
    }

    if (internal_CanRecordProcess(id)) {
      // Accumulate in the child process if needed.
      if (!XRE_IsParentProcess()) {
        const ScalarInfo &info = gScalars[static_cast<uint32_t>(id)];
        TelemetryIPCAccumulator::RecordChildKeyedScalarAction(
          id, aKey, info.kind, ScalarActionType::eAdd, unpackedVal);
        return NS_OK;
      }

      // Finally get the scalar.
      KeyedScalar* scalar = nullptr;
      rv = internal_GetKeyedScalarByEnum(id, GeckoProcessType_Default, &scalar);
      if (NS_FAILED(rv)) {
        // Don't throw on expired scalars.
        if (rv == NS_ERROR_NOT_AVAILABLE) {
          return NS_OK;
        }
        return rv;
      }

      sr = scalar->AddValue(aKey, unpackedVal);
    } else {
      sr = ScalarResult::CannotRecordInProcess;
    }
  }

  // Warn the user about the error if we need to.
  if (internal_ShouldLogError(sr)) {
    internal_LogScalarError(aName, sr);
  }

  return MapToNsResult(sr);
}

/**
 * Adds the value to the given scalar.
 *
 * @param aId The scalar enum id.
 * @param aVal The numeric value to add to the scalar.
 */
void
TelemetryScalar::Add(mozilla::Telemetry::ScalarID aId, uint32_t aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildScalarAction(aId, info.kind, ScalarActionType::eAdd,
                                                     scalarValue);
    return;
  }

  ScalarBase* scalar = internal_GetRecordableScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->AddValue(aValue);
}

/**
 * Adds the value to the given keyed scalar.
 *
 * @param aId The scalar enum id.
 * @param aKey The key name.
 * @param aVal The numeric value to add to the scalar.
 */
void
TelemetryScalar::Add(mozilla::Telemetry::ScalarID aId, const nsAString& aKey,
                     uint32_t aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildKeyedScalarAction(
      aId, aKey, info.kind, ScalarActionType::eAdd, scalarValue);
    return;
  }

  KeyedScalar* scalar = internal_GetRecordableKeyedScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->AddValue(aKey, aValue);
}

/**
 * Sets the scalar to the given value.
 *
 * @param aName The scalar name.
 * @param aVal The value to set the scalar to.
 * @param aCx The JS context.
 * @return NS_OK if the value was added or if we're not allow to record to this
 *  dataset. Otherwise, return an error.
 */
nsresult
TelemetryScalar::Set(const nsACString& aName, JS::HandleValue aVal, JSContext* aCx)
{
  // Unpack the aVal to nsIVariant. This uses the JS context.
  nsCOMPtr<nsIVariant> unpackedVal;
  nsresult rv =
    nsContentUtils::XPConnect()->JSToVariant(aCx, aVal,  getter_AddRefs(unpackedVal));
  if (NS_FAILED(rv)) {
    return rv;
  }

  ScalarResult sr;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);

    mozilla::Telemetry::ScalarID id;
    rv = internal_GetEnumByScalarName(aName, &id);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // We're trying to set a plain scalar, so make sure this is one.
    if (internal_IsKeyedScalar(id)) {
      return NS_ERROR_ILLEGAL_VALUE;
    }

    // Are we allowed to record this scalar?
    if (!internal_CanRecordForScalarID(id)) {
      return NS_OK;
    }

    if (internal_CanRecordProcess(id)) {
      // Accumulate in the child process if needed.
      if (!XRE_IsParentProcess()) {
        const ScalarInfo &info = gScalars[static_cast<uint32_t>(id)];
        TelemetryIPCAccumulator::RecordChildScalarAction(id, info.kind, ScalarActionType::eSet,
                                                         unpackedVal);
        return NS_OK;
      }

      // Finally get the scalar.
      ScalarBase* scalar = nullptr;
      rv = internal_GetScalarByEnum(id, GeckoProcessType_Default, &scalar);
      if (NS_FAILED(rv)) {
        // Don't throw on expired scalars.
        if (rv == NS_ERROR_NOT_AVAILABLE) {
          return NS_OK;
        }
        return rv;
      }

      sr = scalar->SetValue(unpackedVal);
    } else {
      sr = ScalarResult::CannotRecordInProcess;
    }
  }

  // Warn the user about the error if we need to.
  if (internal_ShouldLogError(sr)) {
    internal_LogScalarError(aName, sr);
  }

  return MapToNsResult(sr);
}

/**
 * Sets the keyed scalar to the given value.
 *
 * @param aName The scalar name.
 * @param aKey The key name.
 * @param aVal The value to set the scalar to.
 * @param aCx The JS context.
 * @return NS_OK if the value was added or if we're not allow to record to this
 *  dataset. Otherwise, return an error.
 */
nsresult
TelemetryScalar::Set(const nsACString& aName, const nsAString& aKey, JS::HandleValue aVal,
                     JSContext* aCx)
{
  // Unpack the aVal to nsIVariant. This uses the JS context.
  nsCOMPtr<nsIVariant> unpackedVal;
  nsresult rv =
    nsContentUtils::XPConnect()->JSToVariant(aCx, aVal,  getter_AddRefs(unpackedVal));
  if (NS_FAILED(rv)) {
    return rv;
  }

  ScalarResult sr;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);

    mozilla::Telemetry::ScalarID id;
    rv = internal_GetEnumByScalarName(aName, &id);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // We're trying to set a keyed scalar. Report an error if this isn't one.
    if (!internal_IsKeyedScalar(id)) {
      return NS_ERROR_ILLEGAL_VALUE;
    }

    // Are we allowed to record this scalar?
    if (!internal_CanRecordForScalarID(id)) {
      return NS_OK;
    }

    if (internal_CanRecordProcess(id)) {
      // Accumulate in the child process if needed.
      if (!XRE_IsParentProcess()) {
        const ScalarInfo &info = gScalars[static_cast<uint32_t>(id)];
        TelemetryIPCAccumulator::RecordChildKeyedScalarAction(
          id, aKey, info.kind, ScalarActionType::eSet, unpackedVal);
        return NS_OK;
      }

      // Finally get the scalar.
      KeyedScalar* scalar = nullptr;
      rv = internal_GetKeyedScalarByEnum(id, GeckoProcessType_Default, &scalar);
      if (NS_FAILED(rv)) {
        // Don't throw on expired scalars.
        if (rv == NS_ERROR_NOT_AVAILABLE) {
          return NS_OK;
        }
        return rv;
      }

      sr = scalar->SetValue(aKey, unpackedVal);
    } else {
      sr = ScalarResult::CannotRecordInProcess;
    }
  }

  // Warn the user about the error if we need to.
  if (internal_ShouldLogError(sr)) {
    internal_LogScalarError(aName, sr);
  }

  return MapToNsResult(sr);
}

/**
 * Sets the scalar to the given numeric value.
 *
 * @param aId The scalar enum id.
 * @param aValue The numeric, unsigned value to set the scalar to.
 */
void
TelemetryScalar::Set(mozilla::Telemetry::ScalarID aId, uint32_t aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildScalarAction(aId, info.kind, ScalarActionType::eSet,
                                                     scalarValue);
    return;
  }

  ScalarBase* scalar = internal_GetRecordableScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->SetValue(aValue);
}

/**
 * Sets the scalar to the given string value.
 *
 * @param aId The scalar enum id.
 * @param aValue The string value to set the scalar to.
 */
void
TelemetryScalar::Set(mozilla::Telemetry::ScalarID aId, const nsAString& aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildScalarAction(aId, info.kind, ScalarActionType::eSet,
                                                     scalarValue);
    return;
  }

  ScalarBase* scalar = internal_GetRecordableScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->SetValue(aValue);
}

/**
 * Sets the scalar to the given boolean value.
 *
 * @param aId The scalar enum id.
 * @param aValue The boolean value to set the scalar to.
 */
void
TelemetryScalar::Set(mozilla::Telemetry::ScalarID aId, bool aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildScalarAction(aId, info.kind, ScalarActionType::eSet,
                                                     scalarValue);
    return;
  }

  ScalarBase* scalar = internal_GetRecordableScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->SetValue(aValue);
}

/**
 * Sets the keyed scalar to the given numeric value.
 *
 * @param aId The scalar enum id.
 * @param aKey The scalar key.
 * @param aValue The numeric, unsigned value to set the scalar to.
 */
void
TelemetryScalar::Set(mozilla::Telemetry::ScalarID aId, const nsAString& aKey,
                     uint32_t aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildKeyedScalarAction(
      aId, aKey, info.kind, ScalarActionType::eSet, scalarValue);
    return;
  }

  KeyedScalar* scalar = internal_GetRecordableKeyedScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->SetValue(aKey, aValue);
}

/**
 * Sets the scalar to the given boolean value.
 *
 * @param aId The scalar enum id.
 * @param aKey The scalar key.
 * @param aValue The boolean value to set the scalar to.
 */
void
TelemetryScalar::Set(mozilla::Telemetry::ScalarID aId, const nsAString& aKey,
                     bool aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildKeyedScalarAction(
      aId, aKey, info.kind, ScalarActionType::eSet, scalarValue);
    return;
  }

  KeyedScalar* scalar = internal_GetRecordableKeyedScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->SetValue(aKey, aValue);
}

/**
 * Sets the scalar to the maximum of the current and the passed value.
 *
 * @param aName The scalar name.
 * @param aVal The numeric value to set the scalar to.
 * @param aCx The JS context.
 * @return NS_OK if the value was added or if we're not allow to record to this
 *  dataset. Otherwise, return an error.
 */
nsresult
TelemetryScalar::SetMaximum(const nsACString& aName, JS::HandleValue aVal, JSContext* aCx)
{
  // Unpack the aVal to nsIVariant. This uses the JS context.
  nsCOMPtr<nsIVariant> unpackedVal;
  nsresult rv =
    nsContentUtils::XPConnect()->JSToVariant(aCx, aVal,  getter_AddRefs(unpackedVal));
  if (NS_FAILED(rv)) {
    return rv;
  }

  ScalarResult sr;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);

    mozilla::Telemetry::ScalarID id;
    rv = internal_GetEnumByScalarName(aName, &id);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // Make sure this is not a keyed scalar.
    if (internal_IsKeyedScalar(id)) {
      return NS_ERROR_ILLEGAL_VALUE;
    }

    // Are we allowed to record this scalar?
    if (!internal_CanRecordForScalarID(id)) {
      return NS_OK;
    }

    if (internal_CanRecordProcess(id)) {
      // Accumulate in the child process if needed.
      if (!XRE_IsParentProcess()) {
        const ScalarInfo &info = gScalars[static_cast<uint32_t>(id)];
        TelemetryIPCAccumulator::RecordChildScalarAction(id, info.kind, ScalarActionType::eSetMaximum,
                                                         unpackedVal);
        return NS_OK;
      }

      // Finally get the scalar.
      ScalarBase* scalar = nullptr;
      rv = internal_GetScalarByEnum(id, GeckoProcessType_Default, &scalar);
      if (NS_FAILED(rv)) {
        // Don't throw on expired scalars.
        if (rv == NS_ERROR_NOT_AVAILABLE) {
          return NS_OK;
        }
        return rv;
      }

      sr = scalar->SetMaximum(unpackedVal);
    } else {
      sr = ScalarResult::CannotRecordInProcess;
    }
  }

  // Warn the user about the error if we need to.
  if (internal_ShouldLogError(sr)) {
    internal_LogScalarError(aName, sr);
  }

  return MapToNsResult(sr);
}

/**
 * Sets the scalar to the maximum of the current and the passed value.
 *
 * @param aName The scalar name.
 * @param aKey The key name.
 * @param aVal The numeric value to set the scalar to.
 * @param aCx The JS context.
 * @return NS_OK if the value was added or if we're not allow to record to this
 *  dataset. Otherwise, return an error.
 */
nsresult
TelemetryScalar::SetMaximum(const nsACString& aName, const nsAString& aKey, JS::HandleValue aVal,
                            JSContext* aCx)
{
  // Unpack the aVal to nsIVariant. This uses the JS context.
  nsCOMPtr<nsIVariant> unpackedVal;
  nsresult rv =
    nsContentUtils::XPConnect()->JSToVariant(aCx, aVal,  getter_AddRefs(unpackedVal));
  if (NS_FAILED(rv)) {
    return rv;
  }

  ScalarResult sr;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);

    mozilla::Telemetry::ScalarID id;
    rv = internal_GetEnumByScalarName(aName, &id);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // Make sure this is a keyed scalar.
    if (!internal_IsKeyedScalar(id)) {
      return NS_ERROR_ILLEGAL_VALUE;
    }

    // Are we allowed to record this scalar?
    if (!internal_CanRecordForScalarID(id)) {
      return NS_OK;
    }

    if (internal_CanRecordProcess(id)) {
      // Accumulate in the child process if needed.
      if (!XRE_IsParentProcess()) {
        const ScalarInfo &info = gScalars[static_cast<uint32_t>(id)];
        TelemetryIPCAccumulator::RecordChildKeyedScalarAction(
          id, aKey, info.kind, ScalarActionType::eSetMaximum, unpackedVal);
        return NS_OK;
      }

      // Finally get the scalar.
      KeyedScalar* scalar = nullptr;
      rv = internal_GetKeyedScalarByEnum(id, GeckoProcessType_Default, &scalar);
      if (NS_FAILED(rv)) {
        // Don't throw on expired scalars.
        if (rv == NS_ERROR_NOT_AVAILABLE) {
          return NS_OK;
        }
        return rv;
      }

      sr = scalar->SetMaximum(aKey, unpackedVal);
    } else {
      sr = ScalarResult::CannotRecordInProcess;
    }
  }

  // Warn the user about the error if we need to.
  if (internal_ShouldLogError(sr)) {
    internal_LogScalarError(aName, sr);
  }

  return MapToNsResult(sr);
}

/**
 * Sets the scalar to the maximum of the current and the passed value.
 *
 * @param aId The scalar enum id.
 * @param aValue The numeric value to set the scalar to.
 */
void
TelemetryScalar::SetMaximum(mozilla::Telemetry::ScalarID aId, uint32_t aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildScalarAction(aId, info.kind, ScalarActionType::eSetMaximum,
                                                     scalarValue);
    return;
  }

  ScalarBase* scalar = internal_GetRecordableScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->SetMaximum(aValue);
}

/**
 * Sets the keyed scalar to the maximum of the current and the passed value.
 *
 * @param aId The scalar enum id.
 * @param aKey The key name.
 * @param aValue The numeric value to set the scalar to.
 */
void
TelemetryScalar::SetMaximum(mozilla::Telemetry::ScalarID aId, const nsAString& aKey,
                            uint32_t aValue)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);

  // Accumulate in the child process if needed.
  if (!XRE_IsParentProcess()) {
    nsCOMPtr<nsIVariant> scalarValue;
    nsresult rv = GetVariant(aValue, scalarValue);
    if (NS_FAILED(rv)) {
      return;
    }
    const ScalarInfo &info = gScalars[static_cast<uint32_t>(aId)];
    TelemetryIPCAccumulator::RecordChildKeyedScalarAction(
      aId, aKey, info.kind, ScalarActionType::eSetMaximum, scalarValue);
    return;
  }

  KeyedScalar* scalar = internal_GetRecordableKeyedScalar(aId);
  if (!scalar) {
    return;
  }

  scalar->SetMaximum(aKey, aValue);
}

/**
 * Serializes the scalars from the given dataset to a json-style object and resets them.
 * The returned structure looks like:
 *    {"process": {"group1.probe":1,"group1.other_probe":false,...}, ... }.
 *
 * @param aDataset DATASET_RELEASE_CHANNEL_OPTOUT or DATASET_RELEASE_CHANNEL_OPTIN.
 * @param aClear Whether to clear out the scalars after snapshotting.
 */
nsresult
TelemetryScalar::CreateSnapshots(unsigned int aDataset, bool aClearScalars, JSContext* aCx,
                                 uint8_t optional_argc, JS::MutableHandle<JS::Value> aResult)
{
  MOZ_ASSERT(XRE_IsParentProcess(),
             "Snapshotting scalars should only happen in the parent processes.");
  // If no arguments were passed in, apply the default value.
  if (!optional_argc) {
    aClearScalars = false;
  }

  JS::Rooted<JSObject*> root_obj(aCx, JS_NewPlainObject(aCx));
  if (!root_obj) {
    return NS_ERROR_FAILURE;
  }
  aResult.setObject(*root_obj);

  // Return `{}` in child processes.
  if (!XRE_IsParentProcess()) {
    return NS_OK;
  }

  // Only lock the mutex while accessing our data, without locking any JS related code.
  typedef mozilla::Pair<const char*, nsCOMPtr<nsIVariant>> DataPair;
  typedef nsTArray<DataPair> ScalarArray;
  nsDataHashtable<ProcessIDHashKey, ScalarArray> scalarsToReflect;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);
    // Iterate the scalars in gScalarStorageMap. The storage may contain empty or yet to be
    // initialized scalars from all the supported processes.
    for (auto iter = gScalarStorageMap.Iter(); !iter.Done(); iter.Next()) {
      ScalarStorageMapType* scalarStorage = static_cast<ScalarStorageMapType*>(iter.Data());
      ScalarArray& processScalars = scalarsToReflect.GetOrInsert(iter.Key());

      // Iterate each available child storage.
      for (auto childIter = scalarStorage->Iter(); !childIter.Done(); childIter.Next()) {
        ScalarBase* scalar = static_cast<ScalarBase*>(childIter.Data());

        // Get the informations for this scalar.
        const ScalarInfo& info = gScalars[childIter.Key()];

        // Serialize the scalar if it's in the desired dataset.
        if (IsInDataset(info.dataset, aDataset)) {
          // Get the scalar value.
          nsCOMPtr<nsIVariant> scalarValue;
          nsresult rv = scalar->GetValue(scalarValue);
          if (NS_FAILED(rv)) {
            return rv;
          }
          // Append it to our list.
          processScalars.AppendElement(mozilla::MakePair(info.name(), scalarValue));
        }
      }
    }

    if (aClearScalars) {
      // The map already takes care of freeing the allocated memory.
      gScalarStorageMap.Clear();
    }
  }

  // Reflect it to JS.
  for (auto iter = scalarsToReflect.Iter(); !iter.Done(); iter.Next()) {
    ScalarArray& processScalars = iter.Data();
    const char* processName =
      XRE_ChildProcessTypeToString(static_cast<GeckoProcessType>(iter.Key()));

    // Create the object that will hold the scalars for this process and add it
    // to the returned root object.
    JS::RootedObject processObj(aCx, JS_NewPlainObject(aCx));
    if (!processObj ||
        !JS_DefineProperty(aCx, root_obj, processName, processObj, JSPROP_ENUMERATE)) {
      return NS_ERROR_FAILURE;
    }

    for (nsTArray<DataPair>::size_type i = 0; i < processScalars.Length(); i++) {
      const DataPair& scalar = processScalars[i];

      // Convert it to a JS Val.
      JS::Rooted<JS::Value> scalarJsValue(aCx);
      nsresult rv =
        nsContentUtils::XPConnect()->VariantToJS(aCx, processObj, scalar.second(), &scalarJsValue);
      if (NS_FAILED(rv)) {
        return rv;
      }

      // Add it to the scalar object.
      if (!JS_DefineProperty(aCx, processObj, scalar.first(), scalarJsValue, JSPROP_ENUMERATE)) {
        return NS_ERROR_FAILURE;
      }
    }
  }

  return NS_OK;
}

/**
 * Serializes the scalars from the given dataset to a json-style object and resets them.
 * The returned structure looks like:
 *   { "process": { "group1.probe": { "key_1": 2, "key_2": 1, ... }, ... }, ... }
 *
 * @param aDataset DATASET_RELEASE_CHANNEL_OPTOUT or DATASET_RELEASE_CHANNEL_OPTIN.
 * @param aClear Whether to clear out the keyed scalars after snapshotting.
 */
nsresult
TelemetryScalar::CreateKeyedSnapshots(unsigned int aDataset, bool aClearScalars, JSContext* aCx,
                                      uint8_t optional_argc, JS::MutableHandle<JS::Value> aResult)
{
  MOZ_ASSERT(XRE_IsParentProcess(),
             "Snapshotting scalars should only happen in the parent processes.");
  // If no arguments were passed in, apply the default value.
  if (!optional_argc) {
    aClearScalars = false;
  }

  JS::Rooted<JSObject*> root_obj(aCx, JS_NewPlainObject(aCx));
  if (!root_obj) {
    return NS_ERROR_FAILURE;
  }
  aResult.setObject(*root_obj);

  // Return `{}` in child processes.
  if (!XRE_IsParentProcess()) {
    return NS_OK;
  }

  // Only lock the mutex while accessing our data, without locking any JS related code.
  typedef mozilla::Pair<const char*, nsTArray<KeyedScalar::KeyValuePair>> DataPair;
  typedef nsTArray<DataPair> ScalarArray;
  nsDataHashtable<ProcessIDHashKey, ScalarArray> scalarsToReflect;
  {
    StaticMutexAutoLock locker(gTelemetryScalarsMutex);
    // Iterate the scalars in gKeyedScalarStorageMap. The storage may contain empty or yet
    // to be initialized scalars from all the supported processes.
    for (auto iter = gKeyedScalarStorageMap.Iter(); !iter.Done(); iter.Next()) {
      KeyedScalarStorageMapType* scalarStorage =
        static_cast<KeyedScalarStorageMapType*>(iter.Data());
      ScalarArray& processScalars = scalarsToReflect.GetOrInsert(iter.Key());

      for (auto childIter = scalarStorage->Iter(); !childIter.Done(); childIter.Next()) {
        KeyedScalar* scalar = static_cast<KeyedScalar*>(childIter.Data());

        // Get the informations for this scalar.
        const ScalarInfo& info = gScalars[childIter.Key()];

        // Serialize the scalar if it's in the desired dataset.
        if (IsInDataset(info.dataset, aDataset)) {
          // Get the keys for this scalar.
          nsTArray<KeyedScalar::KeyValuePair> scalarKeyedData;
          nsresult rv = scalar->GetValue(scalarKeyedData);
          if (NS_FAILED(rv)) {
            return rv;
          }
          // Append it to our list.
          processScalars.AppendElement(mozilla::MakePair(info.name(), scalarKeyedData));
        }
      }
    }

    if (aClearScalars) {
      // The map already takes care of freeing the allocated memory.
      gKeyedScalarStorageMap.Clear();
    }
  }

  // Reflect it to JS.
  for (auto iter = scalarsToReflect.Iter(); !iter.Done(); iter.Next()) {
    ScalarArray& processScalars = iter.Data();
    const char* processName =
      XRE_ChildProcessTypeToString(static_cast<GeckoProcessType>(iter.Key()));

    // Create the object that will hold the scalars for this process and add it
    // to the returned root object.
    JS::RootedObject processObj(aCx, JS_NewPlainObject(aCx));
    if (!processObj ||
        !JS_DefineProperty(aCx, root_obj, processName, processObj, JSPROP_ENUMERATE)) {
      return NS_ERROR_FAILURE;
    }

    for (nsTArray<DataPair>::size_type i = 0; i < processScalars.Length(); i++) {
      const DataPair& keyedScalarData = processScalars[i];

      // Go through each keyed scalar and create a keyed scalar object.
      // This object will hold the values for all the keyed scalar keys.
      JS::RootedObject keyedScalarObj(aCx, JS_NewPlainObject(aCx));

      // Define a property for each scalar key, then add it to the keyed scalar
      // object.
      const nsTArray<KeyedScalar::KeyValuePair>& keyProps = keyedScalarData.second();
      for (uint32_t i = 0; i < keyProps.Length(); i++) {
        const KeyedScalar::KeyValuePair& keyData = keyProps[i];

        // Convert the value for the key to a JSValue.
        JS::Rooted<JS::Value> keyJsValue(aCx);
        nsresult rv =
          nsContentUtils::XPConnect()->VariantToJS(aCx, keyedScalarObj, keyData.second(), &keyJsValue);
        if (NS_FAILED(rv)) {
          return rv;
        }

        // Add the key to the scalar representation.
        const NS_ConvertUTF8toUTF16 key(keyData.first());
        if (!JS_DefineUCProperty(aCx, keyedScalarObj, key.Data(), key.Length(), keyJsValue, JSPROP_ENUMERATE)) {
          return NS_ERROR_FAILURE;
        }
      }

      // Add the scalar to the root object.
      if (!JS_DefineProperty(aCx, processObj, keyedScalarData.first(), keyedScalarObj, JSPROP_ENUMERATE)) {
        return NS_ERROR_FAILURE;
      }
    }
  }

  return NS_OK;
}

/**
 * Resets all the stored scalars. This is intended to be only used in tests.
 */
void
TelemetryScalar::ClearScalars()
{
  MOZ_ASSERT(XRE_IsParentProcess(), "Scalars should only be cleared in the parent process.");
  if (!XRE_IsParentProcess()) {
    return;
  }

  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  gScalarStorageMap.Clear();
  gKeyedScalarStorageMap.Clear();
}

size_t
TelemetryScalar::GetMapShallowSizesOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  return gScalarNameIDMap.ShallowSizeOfExcludingThis(aMallocSizeOf);
}

size_t
TelemetryScalar::GetScalarSizesOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf)
{
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  size_t n = 0;
  // Account for scalar data coming from parent and child processes.
  for (auto iter = gScalarStorageMap.Iter(); !iter.Done(); iter.Next()) {
    ScalarStorageMapType* scalarStorage = static_cast<ScalarStorageMapType*>(iter.Data());
    for (auto childIter = scalarStorage->Iter(); !childIter.Done(); childIter.Next()) {
      ScalarBase* scalar = static_cast<ScalarBase*>(childIter.Data());
      n += scalar->SizeOfIncludingThis(aMallocSizeOf);
    }
  }
  // Also account for keyed scalar data coming from parent and child processes.
  for (auto iter = gKeyedScalarStorageMap.Iter(); !iter.Done(); iter.Next()) {
    KeyedScalarStorageMapType* scalarStorage =
      static_cast<KeyedScalarStorageMapType*>(iter.Data());
    for (auto childIter = scalarStorage->Iter(); !childIter.Done(); childIter.Next()) {
      KeyedScalar* scalar = static_cast<KeyedScalar*>(childIter.Data());
      n += scalar->SizeOfIncludingThis(aMallocSizeOf);
    }
  }
  return n;
}

void
TelemetryScalar::UpdateChildData(GeckoProcessType aProcessType,
                                 const nsTArray<mozilla::Telemetry::ScalarAction>& aScalarActions)
{
  MOZ_ASSERT(XRE_IsParentProcess(),
             "The stored child processes scalar data must be updated from the parent process.");
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  if (!internal_CanRecordBase()) {
    return;
  }

  for (auto& upd : aScalarActions) {
    if (internal_IsKeyedScalar(upd.mId)) {
      continue;
    }

    // Are we allowed to record this scalar? We don't need to check for
    // allowed processes here, that's taken care of when recording
    // in child processes.
    if (!internal_CanRecordForScalarID(upd.mId)) {
      continue;
    }

    // Refresh the data in the parent process with the data coming from the child
    // processes.
    ScalarBase* scalar = nullptr;
    nsresult rv = internal_GetScalarByEnum(upd.mId, aProcessType, &scalar);
    if (NS_FAILED(rv)) {
      NS_WARNING("NS_FAILED internal_GetScalarByEnum for CHILD");
      continue;
    }

    switch (upd.mActionType)
    {
      case ScalarActionType::eSet:
        scalar->SetValue(upd.mData);
        break;
      case ScalarActionType::eAdd:
        scalar->AddValue(upd.mData);
        break;
      case ScalarActionType::eSetMaximum:
        scalar->SetMaximum(upd.mData);
        break;
      default:
        NS_WARNING("Unsupported action coming from scalar child updates.");
    }
  }
}

void
TelemetryScalar::UpdateChildKeyedData(GeckoProcessType aProcessType,
                                      const nsTArray<mozilla::Telemetry::KeyedScalarAction>& aScalarActions)
{
  MOZ_ASSERT(XRE_IsParentProcess(),
             "The stored child processes keyed scalar data must be updated from the parent process.");
  StaticMutexAutoLock locker(gTelemetryScalarsMutex);
  if (!internal_CanRecordBase()) {
    return;
  }

  for (auto& upd : aScalarActions) {
    if (!internal_IsKeyedScalar(upd.mId)) {
      continue;
    }

    // Are we allowed to record this scalar? We don't need to check for
    // allowed processes here, that's taken care of when recording
    // in child processes.
    if (!internal_CanRecordForScalarID(upd.mId)) {
      continue;
    }

    // Refresh the data in the parent process with the data coming from the child
    // processes.
    KeyedScalar* scalar = nullptr;
    nsresult rv = internal_GetKeyedScalarByEnum(upd.mId, aProcessType, &scalar);
    if (NS_FAILED(rv)) {
      NS_WARNING("NS_FAILED internal_GetScalarByEnum for CHILD");
      continue;
    }

    switch (upd.mActionType)
    {
      case ScalarActionType::eSet:
        scalar->SetValue(NS_ConvertUTF8toUTF16(upd.mKey), upd.mData);
        break;
      case ScalarActionType::eAdd:
        scalar->AddValue(NS_ConvertUTF8toUTF16(upd.mKey), upd.mData);
        break;
      case ScalarActionType::eSetMaximum:
        scalar->SetMaximum(NS_ConvertUTF8toUTF16(upd.mKey), upd.mData);
        break;
      default:
        NS_WARNING("Unsupported action coming from keyed scalar child updates.");
    }
  }
}
