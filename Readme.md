Here we are using Secondary non unique index which is not necessarily unique and contain duplicate values and is used to speed up queries such as searching by city

Secondary index — usually NOT unique
Points to a set of rows matching a value. user_id on the orders table is intentionally non-unique — one user places many orders. That is exactly what makes it a useful secondary index candidate.