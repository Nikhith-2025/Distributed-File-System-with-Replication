# instructions to verify the bonus part 
CREATEFOLDER <foldername>
  # creating a file inside a folder 
    CREATE <foldername>/<filename>
    and for the rest of the command if u want to use this file then use the total address <foldername>/<filename>
  # to move a file from one folder to another
    MOVE <foldername1>/<filename> <foldername2>
  # view a folder
    VIEWFOLDER <foldername>
    and the root is represented using / 
    so if u want to take a file out of the folder then move it to /

  # checkpoint part
    # creating a checkpoint
      CHECKPOINT <filename(total name if it is in a folder)> <checkpoint(name (it can be something random))>
    # to see all the checkpoints
      LISTCHECKPOINTS <filename>
    # to see what a file has at a particular checkpoint
      VIEWCHECKPOINT <filename> <checkpoint(name)>
    # to revert the changes to a particular checkpoint
      REVERT <filename> <checkpoint(name)>
     u can use read after revert to see if the file is actually reverted or not

  # Request accessing 
    # to request the owner for a particular request
      REQUESTACCESS <filename> READ
      REQUESTACCESS <filename> WRITE

    # to check if u got any request from others
      VIEWREQUESTS <filename>
    # approval / rejection
      APPROVEREQUEST <filename> <username>
      DENYREQUEST <filename> <username>
      