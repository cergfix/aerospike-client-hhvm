#include "scan_operation.h"
#include "conversions.h"
#include "ext_aerospike.h"

#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/program-functions.h"

namespace HPHP {

    /*
     *******************************************************************************************
     * Callback function for each record scanned by aerospike_scan_foreach() API
     *
     * @param val_p         An as_val of record type
     * @param udata_p       foreach_callback_user_udata pointer which contains
     *                      PHP user callback function and as_error reference
     *                      to be populated in case of error.
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    bool scan_query_callback(const as_val *val_p, void *udata)
    {
        if (!val_p) {
            return false;
        }

        as_record       *record_p = as_record_fromval(val_p);

        if (!record_p) {
            return false;
        }

        pthread_rwlock_wrlock(&scan_query_callback_mutex);
        if (g_context.isNull()) {
            hphp_session_init();
        }
        Array           temp_php_record = Array::Create();
        foreach_callback_user_udata      *conversion_data_p = (foreach_callback_user_udata *)udata;

        as_record_to_php_record(record_p, &record_p->key, temp_php_record, NULL, conversion_data_p->error);

        Array php_record = Array::Create();
        php_record.append(temp_php_record);
        Variant ret = vm_call_user_func(conversion_data_p->function, php_record);

        bool do_continue = true;
        if (ret.isBoolean() && ret.toBoolean() == false) {
            do_continue = false;
        }

        pthread_rwlock_unlock(&scan_query_callback_mutex);

        return do_continue;
    }

    /*
     *******************************************************************************************
     * Function to initialize as_scan structure for calling aerospike_scan_foreach() API
     *
     * @param scan          An as_scan pointer
     * @param ns            Namespace to be scanned
     * @param set           Set to be scanned
     * @param bins          Array of Bins to be scanned
     * @param error         as_error reference to be populated by this function
     *                      in case of error
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    as_status initialize_scan(as_scan *scan, const Variant &ns, const Variant &set,
            const Variant &bins, as_error &error)
    {
        as_error_reset(&error);

        if (!ns.isString() || ns.toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Namespace/Set must be non empty string");
        } else if (!set.isNull() && (!set.isString() || set.toString().empty())) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Set must be NULL or non empty string");
        } else if (!bins.isNull() && !bins.isArray()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Bin names must be an Array");
        } else {
            //Initialize scan structure
            as_scan_init(scan, ns.toString().c_str(), set.toString().c_str());
            //Add bins to structure if presents
            if (!bins.isNull()) {
                Array bin_array = bins.toArray();
                //Initialize select bins only when argument contails bins
                if (bin_array.length() > 0) {
                    as_scan_select_init(scan, bin_array.length());
                    for (ArrayIter iter(bin_array); iter; ++iter) {
                        if (!iter.second().isArray()) {
                            as_scan_select(scan, iter.second().toString().c_str());
                        } else {
                            //In case bin names are arrays currently we are ignoring
                            //those bins
                            continue;
                            //For generating error for above case, enable below
                            //error code
                            //as_error_update(&error, AEROSPIKE_ERR_PARAM,
                            //        "Bin names must be an Array of Strings");
                            //break;
                        }
                    }
                }
            }
        }

        return error.code;
    }

    /*
     *******************************************************************************************
     * Function to initialize as_scan structure for calling
     * aerospike_scan_background() API
     *
     * @param scan              An as_scan pointer
     * @param ns                Namespace to be scanned
     * @param set               Set to be scanned
     * @param module            Module registered in DB
     * @param function          Function defined in above registered module
     * @param args              List of arguments to the above defined function
     * @param static_pool       StaticPoolManager instance reference, to be used for
     *                          the conversion lifecycle
     * @param serializer_type   The serializer type to be used to support
     *                          server-unsupported datatypes for the UDF's
     *                          argument list.
     * @param error             as_error reference to be populated by this function
     *                          in case of error
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    as_status initialize_scanApply(as_scan *scan, const Variant &ns, const Variant &set,
            const Variant &module, const Variant &function, const Variant &args,
            StaticPoolManager &static_pool, int16_t serializer_type, as_error &error)
    {
        as_list     *args_list = NULL;
        Array       php_args;

        as_error_reset(&error);

        if (!ns.isString() || ns.toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Namespace/Set must not be empty");
        } else if (!set.isNull() && (!set.isString() || set.toString().empty())) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Set must be NULL or non empty string");
        } else if (!module.isString() || module.toString().empty() ||
                !function.isString() || function.toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Module/Function must not be empty");
        } else if (!args.isNull() && !args.isArray()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Function arguments must be of type array");
        } else {
            //Initialize scan structure
            as_scan_init(scan, ns.toString().c_str(), set.toString().c_str());

            if (!args.isNull()) {
                php_args = args.toArray();
            }
            if (!args.isNull() && php_list_to_as_list(php_args, &args_list,
                        static_pool, serializer_type, error)) {
                //Argument list creation for UDF failed
            } else if (!as_scan_apply_each(scan, module.toString().c_str(),
                        function.toString().c_str(), args_list)) {
                as_error_update(&error, AEROSPIKE_ERR_PARAM,
                        "Unable to initiate background scan : UDF apply failed");
            }
        }

        return error.code;
    }

    /*
     *******************************************************************************************
     * Function to construct the EQUALS and CONTAINS where predicate as required as argument
     * in Query and Aggregate API from PHP
     *
     * @param where         Array type to be constructed as required predicate
     * @param bin           Name of the BIN of type String
     * @param value         Value of the BIN of type Integer or String
     * @param index_type    Integer index type supported by aerospike
     * @param isContains    flag to indicate the EQUALS or CONTAINS predicate
     * @return true if success. Otherwise false.
     *******************************************************************************************
     */
    bool construct_Equals_Contains_Predicates(Array &where, const Variant &bin, const Variant &value,
            int64_t index_type/* = 0*/, bool isContains/* = false*/)
    {
        bool        isNull = false;

        if (!bin.isString() || bin.toString().empty()) {
            //Bin name must be non empty string
            isNull = true;
        } else if (!value.isInteger() && !value.isString()) {
            //Bin value must be integer or non empty string
            isNull = true;
        } else {
            where.set(s_bin, bin.toString());
            if (isContains) {
                where.set(s_index_type, index_type);
                where.set(s_op, String("CONTAINS"));
            } else {
                where.set(s_op, String("="));
            }
            if (value.isInteger()) {
                //Integer Value
                where.set(s_val, value.toInt64());
            } else if (value.toString().empty()) {
                //Bin value must be integer or non empty string
                isNull = true;
            } else {
                //String Value
                where.set(s_val, value.toString());
            }
        }

        return isNull;
    }

    /*
     *******************************************************************************************
     * Function to construct the BETWEEN and RANGE where predicate as required as argument
     * in Query and Aggregate API from PHP
     *
     * @param where         Array type to be constructed as required predicate
     * @param bin           Name of the BIN of type String
     * @param min           Min value of type Integer or String
     * @param max           Max value of type Integer or String
     * @param index_type    Integer index type supported by aerospike
     * @param isRange       flag to indicate the BETWEEN or RANGE predicate
     * @return true if success. Otherwise false.
     *******************************************************************************************
     */
    bool construct_Between_Range_Predicates(Array &where, const Variant &bin, const Variant &min,
            const Variant &max, int64_t index_type/* = 0*/, bool isRange/* = false*/)
    {
        Array       val = Array::Create();
        bool        isNull = false;

        if (!bin.isString() || bin.toString().empty()) {
            //Bin name must be non empty string
            isNull = true;
        } else if (!isRange && ((!min.isNull() && !min.isInteger() && !min.isBoolean()) ||
                    (!max.isNull() && !max.isInteger() && !min.isBoolean()))) {
            //Between
            //min and max must be integers
            isNull = true;
        } else {
            where.set(s_bin, bin.toString());
            if (isRange) {
                where.set(s_index_type, index_type);
                where.set(s_op, String("RANGE"));
                if (min.isInteger() && max.isInteger()) {
                    //Integera values
                    val.append(min.toInt64());
                    val.append(max.toInt64());
                    where.set(s_val, val);
                } else if (min.isString() && max.isString()) {
                    //String values
                    val.append(min.toString());
                    val.append(max.toString());
                    where.set(s_val, val);
                } else {
                    //min and max must be integers or strings
                    isNull = true;
                }
            } else {
                where.set(s_op, String("BETWEEN"));
                val.append(min.toInt64());
                val.append(max.toInt64());
                where.set(s_val, val);
            }
        }

        return isNull;
    }

    /*
     *******************************************************************************************
     * Function to initialize as_query structure for calling aerospike_query_foreach() API
     *
     * @param query         An as_query pointer
     * @param ns            Namespace to be queried
     * @param set           Set to be queried
     * @param where         predicates to be applyed to query
     * @param bins          Array of Bins to be queried
     * @param error         as_error reference to be populated by this function
     *                      in case of error
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    as_status initialize_query(as_query *query, const Variant &ns, const Variant &set,
            const Variant &where, const Variant &bins, as_error &error)
    {
        as_error_reset(&error);

        if (!ns.isString() || ns.toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Namespace must be non empty string");
        } else if (!set.isNull() && (!set.isString() || set.toString().empty())) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Set must be NULL or non empty string");
        } else if (!bins.isNull() && !bins.isArray()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Bin names must be an Array");
        } else if (!where.isNull() && !where.isArray()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Predicate must be an Array containing the keys 'bin', ['index_type',] 'op', and 'val'");
        } else {
            //Initialize query structure
            as_query_init(query, ns.toString().c_str(), set.toString().c_str());
            //Add bins to structure if presents
            if (!bins.isNull()) {
                Array bin_array = bins.toArray();
                //Initialize select bins only when argument contails bins
                if (bin_array.length() > 0) {
                    as_query_select_init(query, bin_array.length());
                    for (ArrayIter iter(bin_array); iter; ++iter) {
                        if (!iter.second().isArray()) {
                            as_query_select(query, iter.second().toString().c_str());
                        } else {
                            //In case bin names are arrays currently we are ignoring
                            //those bins
                            continue;
                            //For generating error for above case, enable below
                            //error code
                            //as_error_update(&error, AEROSPIKE_ERR_PARAM,
                            //        "Bin names must be an Array of Strings");
                            //break;
                        }
                    }
                }
            }
            //Add where predicates to query
            if (!where.isNull()) {
                Array       predicate = where.toArray();
                if (predicate.length() > 0) {
                    //NOTE : Validation of error code is required here if select bin
                    //failure condition is enabled
                    if (error.code == AEROSPIKE_OK && isPredicate(predicate, error) == AEROSPIKE_OK) {
                        initialize_where_predicate(query, predicate, error);
                    }
                }
            }
        }

        return error.code;
    }

    /*
     *******************************************************************************************
     * Function to validate where predicate for Query and Aggregate API
     *
     * @param predicate     predicate array to be validated
     * @param error         as_error reference to be populated by this function
     *                      in case of error
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    as_status isPredicate(const Array &predicate, as_error &error)
    {
        as_error_reset(&error);

        if (predicate.exists(s_bin) && predicate.exists(s_index_type) &&
                predicate.exists(s_op) && predicate.exists(s_val)) {
            //Contains or Range predicates
        } else if (predicate.exists(s_bin) && predicate.exists(s_op) &&
                predicate.exists(s_val)) {
            //Equals or Between predicates
        } else {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Predicate must be an Array containing the keys 'bin', ['index_type',] 'op', and 'val'");
        }

        //Continue validations only if required keys are present in the
        //predicates
        if (error.code != AEROSPIKE_OK) {
            //Predicate is invalid and error code is allready set
        } else if (!predicate[s_bin].isString() || predicate[s_bin].toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Predicate must be an Array : 'bin' key value must be non empty string");
        } else if (predicate.exists(s_index_type) && !predicate[s_index_type].isInteger()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Predicate must be an Array : 'index_type' key value must be integer");
        } else if (!predicate[s_op].isString() || predicate[s_op].toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Predicate must be an Array : 'op' key value must be non empty string");
        } else {
            if (predicate[s_val].isArray()) {
                //Between or Range predicates
                Array       val = predicate[s_val].toArray();
                if (!val.exists(0) || !val[0].isInteger() || !val.exists(1) || !val[1].isInteger()) {
                    as_error_update(&error, AEROSPIKE_ERR_PARAM,
                            "Predicate must be an Array : 'val' key value must be an array containing min and max integers");
                }
            } else if (!predicate[s_val].isInteger() && !predicate[s_val].isString()) {
                //Equals or Contains predicate
                as_error_update(&error, AEROSPIKE_ERR_PARAM,
                        "Predicate must be an Array : 'val' key value must be integer or non empty string");
            }
        }

        return error.code;
    }

    /*
     *******************************************************************************************
     * Function to initialize where predicate for Query and Aggregate API
     *
     * @param query         Query object to be used for applying a where
     *                      predicate
     * @param predicate     where predicate to be applyed on query
     * @param error         as_error reference to be populated by this function
     *                      in case of error
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    as_status initialize_where_predicate(as_query *query, const Array &predicate, as_error &error)
    {
        as_error_reset(&error);

        as_query_where_init(query, 1);
        if (predicate[s_op].toString() == String("=")) {
            if (predicate[s_val].isInteger()) {
                as_query_where(query, predicate[s_bin].toString().c_str(),
                        as_integer_equals(predicate[s_val].toInt64()));
            //else if is not needed as is_predicate has done the
            //s_val value validations
            } else {
                as_query_where(query, predicate[s_bin].toString().c_str(),
                        as_string_equals(predicate[s_val].toString().c_str()));
            }
        } else if (predicate[s_op].toString() == String("CONTAINS")) {
            switch (predicate[s_index_type].toInt64())
            {
                case AS_INDEX_TYPE_LIST:
                    if (predicate[s_val].isInteger()) {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_contains(LIST, NUMERIC, predicate[s_val].toInt64()));
                    } else {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_contains(LIST, STRING, predicate[s_val].toString().c_str()));
                    }
                    break;
                case AS_INDEX_TYPE_MAPKEYS:
                    if (predicate[s_val].isInteger()) {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_contains(MAPKEYS, NUMERIC, predicate[s_val].toInt64()));
                    } else {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_contains(MAPKEYS, STRING, predicate[s_val].toString().c_str()));
                    }
                    break;
                case AS_INDEX_TYPE_MAPVALUES:
                    if (predicate[s_val].isInteger()) {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_contains(MAPVALUES, NUMERIC, predicate[s_val].toInt64()));
                    } else {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_contains(MAPVALUES, STRING, predicate[s_val].toString().c_str()));
                    }
                    break;
                default :
                    as_error_update(&error, AEROSPIKE_ERR_PARAM,
                            "Predicate must be an Array : 'index_type' key : Invalid index type");
            }
        } else if (predicate[s_op].toString() == String("BETWEEN")) {
            Array       val = predicate[s_val].toArray();
            as_query_where(query, predicate[s_bin].toString().c_str(),
                    as_integer_range(val[0].toInt64(), val[1].toInt64()));
        } else if (predicate[s_op].toString() == String("RANGE")) {
            Array       val = predicate[s_val].toArray();
            switch (predicate[s_index_type].toInt64())
            {
                case AS_INDEX_TYPE_LIST:
                    if (val[0].isInteger()) {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_range(LIST, NUMERIC, val[0].toInt64(), val[1].toInt64()));
                    } else {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_range(LIST, STRING, val[0].toString().c_str(), val[1].toString().c_str()));
                    }
                    break;
                case AS_INDEX_TYPE_MAPKEYS:
                    if (val[0].isInteger()) {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_range(MAPKEYS, NUMERIC, val[0].toInt64(), val[1].toInt64()));
                    } else {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_range(MAPKEYS, STRING, val[0].toString().c_str(), val[1].toString().c_str()));
                    }
                    break;
                case AS_INDEX_TYPE_MAPVALUES:
                    if (val[0].isInteger()) {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_range(MAPVALUES, NUMERIC, val[0].toInt64(), val[1].toInt64()));
                    } else {
                        as_query_where(query, predicate[s_bin].toString().c_str(),
                                as_range(MAPVALUES, STRING, val[0].toString().c_str(), val[1].toString().c_str()));
                    }
                    break;
                default :
                    as_error_update(&error, AEROSPIKE_ERR_PARAM,
                            "Predicate must be an Array : 'index_type' key : Invalid index type");
            }
        }

        return error.code;
    }

    /*
     *******************************************************************************************
     * Function to initialize as_query structure for calling aerospike_query_foreach() API
     *
     * @param query             An as_query pointer
     * @param ns                Namespace to be queried
     * @param set               Set to be queried
     * @param module            Module registered in DB
     * @param function          Function defined in above registered module
     * @param args              List of arguments to the above defined function
     * @param static_pool       StaticPoolManager instance reference, to be used for
     *                          the conversion lifecycle
     * @param serializer_type   The serializer type to be used to support
     *                          server-unsupported datatypes for the UDF's
     *                          argument list.
     * @param error             as_error reference to be populated by this function
     *                          in case of error
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    as_status initialize_aggregate(as_query *query, const Variant &ns, const Variant &set,
            const Variant &where, const Variant &module, const Variant &function, const Variant &args,
            StaticPoolManager &static_pool, int16_t serializer_type, as_error &error)
    {
        as_list     *args_list = NULL;
        Array       php_args;

        as_error_reset(&error);

        if (!ns.isString() || ns.toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Namespace must be non empty string");
        } else if (!set.isNull() && (!set.isString() || set.toString().empty())) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Set must be NULL or non empty string");
        } else if (!where.isNull() && !where.isArray()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Predicate must be an Array containing the keys 'bin', ['index_type',] 'op', and 'val'");
        } else if (!module.isString() || module.toString().empty() ||
                !function.isString() || function.toString().empty()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Module/Function must not be empty");
        } else if (!args.isNull() && !args.isArray()) {
            as_error_update(&error, AEROSPIKE_ERR_PARAM,
                    "Function arguments must be of type array");
        } else {
            //Initialize query structure
            as_query_init(query, ns.toString().c_str(), set.toString().c_str());
            //Add where predicates to query
            if (!where.isNull()) {
                Array       predicate = where.toArray();
                if (predicate.length() > 0) {
                    if (isPredicate(predicate, error) == AEROSPIKE_OK) {
                        initialize_where_predicate(query, predicate, error);
                    }
                }
            }
            //Apply UDF to query only when where predicates are added
            //successfully or not present
            if (error.code == AEROSPIKE_OK) {
                if (!args.isNull()) {
                    php_args = args.toArray();
                }
                if (!args.isNull() && php_list_to_as_list(php_args, &args_list,
                            static_pool, serializer_type, error)) {
                    //Argument list creation for UDF failed
                } else if (!as_query_apply(query, module.toString().c_str(),
                            function.toString().c_str(), args_list)) {
                    as_error_update(&error, AEROSPIKE_ERR_PARAM,
                            "Unable to initiate aggregate : UDF apply failed");
                }
            }
        }

        return error.code;
    }

    /*
     *******************************************************************************************
     * Callback function for aggregate API
     *
     * @param val_p         An as_val of any type
     * @param udata_p       foreach_callback_udata pointer which wraps us the PHP list
     *                      to be appended to by this function and as_error reference
     *                      to be populated in case of error.
     * @return AEROSPIKE_OK if success. Otherwise AEROSPIKE_ERR_*.
     *******************************************************************************************
     */
    bool aggregate_callback(const as_val *val_p, void *udata)
    {
        bool        do_continue = true;
        Variant     php_value;

        if (!val_p) {
            return false;
        }

        pthread_rwlock_wrlock(&scan_query_callback_mutex);
        if (g_context.isNull()) {
            hphp_session_init();
        }

        foreach_callback_udata      *udata_p = (foreach_callback_udata *)udata;

        if (as_val_to_php_variant(val_p, php_value, udata_p->error) != AEROSPIKE_OK) {
            //Conversion failed stop the aggregate call
            do_continue = false;
        } else {
            if (!php_value.isArray() || !php_value.toArray().empty()) {
                udata_p->data.append(php_value);
            }
        }
        pthread_rwlock_unlock(&scan_query_callback_mutex);

        return do_continue;
    }
}
