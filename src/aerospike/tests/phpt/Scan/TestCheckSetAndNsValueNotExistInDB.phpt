--TEST--
Scan - Set and namespance not exist in DB

--FILE--
<?php
include dirname(__FILE__)."/../../astestframework/astest-phpt-loader.inc";
aerospike_phpt_runtest("Scan", "testCheckSetAndNsValueNotExistInDB");
--EXPECT--
ERR_NAMESPACE_NOT_FOUND

