cmake_minimum_required(VERSION 4.4)

file(STRINGS "${RESULT_LIST}" result_paths ENCODING UTF-8)
set(product_set_ids "")
foreach(result_path IN LISTS result_paths)
    file(READ "${result_path}" receipt)
    # Publish hashes product names, files, bytes, sizes and dependencies into this ID.
    string(JSON product_set_id GET "${receipt}" product_set_id)
    string(APPEND product_set_ids "${product_set_id}\n")
endforeach()
string(SHA256 fingerprint "${product_set_ids}")
file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT "@fingerprint@\n" @ONLY)
